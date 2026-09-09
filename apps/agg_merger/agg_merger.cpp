#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

#include "MappedArray.h"
#include "common.h"
#include "metrics.h"

#include <cassert>
#include <set>
#include <thread>

constexpr bool DEBUG_DISABLE_WRITES = false;

struct PROCESS_POSITIONS_FROM_FIRST : metrics::Counter<uint64_t> {};
struct OUT_POSITIONS : metrics::Counter<uint64_t> {};
struct OUT_RECORDS : metrics::Counter<uint64_t> {};
struct DISCARDED_POSITIONS : metrics::Counter<uint64_t> {};

using AppMetrics =
    metrics::Metrics<PROCESS_POSITIONS_FROM_FIRST, OUT_POSITIONS, OUT_RECORDS, DISCARDED_POSITIONS>;

constexpr size_t WORKERS = 4;

struct FileRange {
  size_t begin;
  size_t end;

  template <typename T> auto beginIt(const MappedArray<T>& records) const {
    return records.begin() + begin;
  }

  template <typename T> auto endIt(const MappedArray<T>& records) const {
    return records.begin() + end;
  }
};

struct WorkerSplit {
  std::vector<FileRange> files;
};

std::array<WorkerSplit, WORKERS>
splitWork(const std::vector<std::unique_ptr<MappedArray<SpillRecord>>>& files) {
  const size_t FILES = files.size();

  if (files.empty())
    throw std::runtime_error("No files");

  const auto& reference = *files.front();

  if (reference.size() == 0)
    throw std::runtime_error("First file is empty");

  /*
    Pick WORKERS-1 boundaries from the first file.
    boundaries[i] is the last record of the zobrist range
    belonging to worker i. Last boundry doesn't exist as it is the last
    entry of each file.
  */

  std::array<uint64_t, WORKERS - 1> boundaries{};

  for (size_t boundary = 0; boundary < WORKERS - 1; ++boundary) {
    const size_t target = (reference.size() * (boundary + 1)) / WORKERS;
    auto it = reference.begin() + target;
    boundaries[boundary] = it->zobrist;
  }

  std::array<WorkerSplit, WORKERS> workerSplits;

  for (size_t worker = 0; worker < WORKERS; ++worker) {
    workerSplits[worker].files.resize(files.size());
  }

  for (auto& fileRange : workerSplits[0].files) {
    fileRange.begin = 0;
  }

  for (size_t worker = 0; worker < WORKERS; ++worker) {
    auto& currentWorkerSplit = workerSplits[worker];

    if (worker != 0) {
      for (size_t fileIdx = 0; fileIdx < FILES; ++fileIdx) {
        currentWorkerSplit.files[fileIdx].begin = workerSplits[worker - 1].files[fileIdx].end;
      }
    }

    for (size_t fileIdx = 0; fileIdx < FILES; ++fileIdx) {
      const auto& records = *files[fileIdx];
      const size_t end = worker == WORKERS - 1
                             ? records.size()
                             : std::lower_bound(records.begin(), records.end(), boundaries[worker],
                                                [](const SpillRecord& r, uint64_t key) {
                                                  return r.zobrist < key;
                                                }) -
                                   records.begin();

      if (worker == WORKERS - 1)
        assert(end == records.size());

      currentWorkerSplit.files[fileIdx].end = end;
    }
  }

  return workerSplits;
}

struct Node {
  size_t file;
  size_t index;
  size_t end;
  SpillRecord rec;
};

struct Cmp {
  bool operator()(const Node& a, const Node& b) const {
    if (a.rec.zobrist != b.rec.zobrist)
      return a.rec.zobrist > b.rec.zobrist;

    if (a.rec.move != b.rec.move)
      return a.rec.move > b.rec.move;

    return a.rec.bucket > b.rec.bucket;
  }
};

void mergeWorker(AppMetrics& metrics, const WorkerSplit& split,
                 const std::vector<std::unique_ptr<MappedArray<SpillRecord>>>& files,
                 std::string outFile) {
  std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("Failed to open " + outFile);

  std::priority_queue<Node, std::vector<Node>, Cmp> pq;

  // One cursor per file.
  for (size_t fileIdx = 0; fileIdx < files.size(); ++fileIdx) {
    const auto& range = split.files[fileIdx];

    if (range.begin == range.end)
      continue;

    const auto& records = *files[fileIdx];

    pq.push({.file = fileIdx, .index = range.begin, .end = range.end, .rec = records[range.begin]});
  }

  std::vector<SpillRecord> group;
  group.reserve(1000);

  std::vector<SpillRecord> merged;
  merged.reserve(1000);

  while (!pq.empty()) {
    const uint64_t zobrist = pq.top().rec.zobrist;
    group.clear();

    // Pull every record belonging to this position.
    while (!pq.empty() && pq.top().rec.zobrist == zobrist) {
      Node node = pq.top();
      pq.pop();

      if (node.file == 0) {
        ++metrics.get<PROCESS_POSITIONS_FROM_FIRST>();
        metrics.maybePrint();
      }

      group.push_back(node.rec);

      const auto& records = *files[node.file];

      ++node.index;

      if (node.index < node.end) {
        node.rec = records[node.index];
        pq.push(node);
      }
    }

    merged.clear();

    for (const SpillRecord& r : group) {
      if (merged.empty() || merged.back().bucket != r.bucket || merged.back().move != r.move) {
        merged.push_back(r);
      } else {
        SpillRecord& m = merged.back();

        m.count += r.count;
        m.white_wins += r.white_wins;
        m.draws += r.draws;
      }
    }

    // std::set<std::pair<uint16_t, uint8_t>> seen;
    //
    // for (const auto& r : merged) {
    //   const auto [it, inserted] = seen.emplace(r.move, r.bucket);
    //   assert(inserted && "Duplicate move/bucket pair in merged records");
    // }

    uint64_t total = 0;
    for (const SpillRecord& r : merged)
      total += r.count;

    if (total < 5) {
      ++metrics.get<DISCARDED_POSITIONS>();
      continue;
    }

    if constexpr (!DEBUG_DISABLE_WRITES) {
      out.write(reinterpret_cast<char*>(merged.data()), merged.size() * sizeof(SpillRecord));
    }

    ++metrics.get<OUT_POSITIONS>();
    metrics.get<OUT_RECORDS>() += merged.size();
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: merge_stats <out> <folder1> [folder2 ...]\n";
    return 1;
  }

  std::string outFile = argv[1];

  std::vector<std::unique_ptr<MappedArray<SpillRecord>>> files;
  std::vector<std::filesystem::path> filepaths;
  for (int i = 2; i < argc; i++)
    for (auto& e : std::filesystem::directory_iterator(argv[i]))
      if (e.is_regular_file()) {
        std::string n = e.path().filename().string();
        if (n.starts_with("agg-") && e.path().extension() == ".bin") {
          filepaths.emplace_back(e.path());
          files.emplace_back(std::make_unique<MappedArray<SpillRecord>>(e.path(), MmapAdvice::RANDOM));
        }
      }

  std::cout << "Found " << files.size() << " files\n";

  auto workSplit = splitWork(files);

  for (auto& file : files) {
    file->advice(MmapAdvice::NORMAL);
  }

  // int i = 0;
  // for (auto& worker : workSplit) {
  //   std::cout << "worker: " << i++ << "\n";
  //   for (auto& file : worker.files) {
  //     std::cout << "  " << file.begin << " " << file.end << std::endl;
  //   }
  // }

  const auto firstSize = (double)files[0]->size();
  AppMetrics appMetrics(
      std::chrono::seconds(1),
      [firstSize](AppMetrics::Snapshot snapshot) {
        std::cout << "estimate % done: "
                  << snapshot.get<PROCESS_POSITIONS_FROM_FIRST>() * 100.0 / firstSize << "% "
                  << "out records: " << snapshot.get<OUT_RECORDS>() << " "
                  << "discarded: " << snapshot.get<DISCARDED_POSITIONS>() << " "
                  << "\n";
      },
      [](AppMetrics::Snapshot snapshot) {
        std::cout << "\n===== Final Metrics =====\n";
        std::cout << "Finished in: "
                  << std::chrono::duration_cast<std::chrono::seconds>(snapshot.getRuntime()).count()
                  << " seconds\n";
        std::cout << "out positions: " << snapshot.get<OUT_POSITIONS>() << "\n";
        std::cout << "out records: " << snapshot.get<OUT_RECORDS>() << "\n";
        std::cout << "discarded: " << snapshot.get<DISCARDED_POSITIONS>() << "\n";
      });

  std::vector<std::thread> workerThreads;
  for (int i = 0; i < WORKERS; ++i) {


    std::string customPath = outFile + "_w" + std::to_string(i);

    workerThreads.emplace_back(mergeWorker, std::ref(appMetrics), std::cref(workSplit[i]),
                               std::cref(files), customPath);
  }

  for (auto& workerThread : workerThreads)
    workerThread.join();

  std::ofstream out(outFile, std::ios::binary);

  constexpr size_t BUFFER_SIZE = 1024 * 1024 * 1024;
  std::vector<char> buffer(BUFFER_SIZE);

  for (int i = 0; i < WORKERS; ++i) {
    std::cout << "merging worker: " << i << "\n";

    std::string path = outFile + "_w" + std::to_string(i);

    std::ifstream in(path, std::ios::binary);

    if (!in)
      throw std::runtime_error("Failed to open " + path);

    while (in) {
      in.read(buffer.data(), buffer.size());
      out.write(buffer.data(), in.gcount());
    }

    in.close();
    std::filesystem::remove(path);
  }
  out.close();
}