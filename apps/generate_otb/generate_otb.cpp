#include "MappedArray.h"
#include "NodeBlobHelpers.h"
#include "common.h"
#include "metrics.h"
#include "misc.h"
#include "position.h"
#include "uci.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

struct POSITIONS_CLASSIFIED : metrics::Counter<uint64_t> {};
struct POSITIONS_WRITTEN : metrics::Counter<uint64_t> {};
struct NODE_BLOBS_WRITTEN : metrics::Counter<uint64_t> {};
struct PREFETCHES_ISSUED : metrics::Counter<uint64_t> {};

using AppMetrics =
    metrics::Metrics<POSITIONS_CLASSIFIED, POSITIONS_WRITTEN, NODE_BLOBS_WRITTEN, PREFETCHES_ISSUED>;

enum class NodeClassification : uint8_t { DROPPED, LEAF, FULL };

namespace {

constexpr size_t DROP_LIMIT = 50;
constexpr size_t LEAF_LIMIT = 150;

// a kept position is never allowed to exceed this many index entries
constexpr size_t MAX_ENTRIES = 200'000'000;

constexpr size_t BUFFER_BYTES = 64ULL * 1024 * 1024;
constexpr size_t RECORDS_PER_BUFFER = BUFFER_BYTES / sizeof(SpillRecord);
constexpr size_t BUFFER_SIZE = RECORDS_PER_BUFFER * sizeof(SpillRecord);

constexpr size_t MAX_DEPTH = 10'000;

// A kept node's records average ~750 bytes and virtually always land inside a
// single page; asking for two means a span that straddles a page boundary still
// arrives in one round trip.
constexpr size_t PREFETCH_BYTES = 8192;

} // namespace

/*
 * Index over the positions that survive DROP_LIMIT.
 *
 * Only kept positions are stored. A key that is absent is DROPPED by
 * definition, which is exactly how an unknown key behaved back when the index
 * held every position in the aggregate. Roughly 92% of the positions in the
 * aggregate are below DROP_LIMIT, so leaving them out shrinks the index by more
 * than an order of magnitude and hands the freed RAM back to the page cache.
 *
 * The visit total is summed during the sequential build pass, which already
 * touches every record. That is what makes the saving possible: classifying a
 * node no longer costs a random read, so the ~2 out of 3 children that turn out
 * to be dropped are resolved entirely in RAM.
 */
class PositionIndex {
public:
  static constexpr size_t NOT_FOUND = std::numeric_limits<size_t>::max();

  void reserve(size_t n) {
    keys.reserve(n);
    slots.reserve(n);
    flags.reserve(n);
  }

  void append(uint64_t key, uint64_t aggOffset, uint64_t visits) {
    assert(visits >= DROP_LIMIT && "dropped positions do not belong in the index");
    assert((keys.empty() || key > keys.back()) && "aggregate is not sorted by key");

    keys.push_back(key);
    slots.push_back(aggOffset);
    flags.push_back(static_cast<uint8_t>(visits < LEAF_LIMIT ? NodeClassification::LEAF
                                                             : NodeClassification::FULL));
  }

  /*
   * Zobrist keys are uniformly distributed, so a direct-address table on their
   * top bits splits the index into buckets of roughly equal size and cuts the
   * search from ~25 probes over hundreds of megabytes to ~4 probes inside a few
   * cache lines. Without it the binary search becomes the next bottleneck as
   * soon as the random reads stop dominating.
   */
  void finalize() {
    if (keys.size() > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("index too large for the prefix table");

    prefixBits = 1;
    while ((1ULL << prefixBits) < keys.size() / TARGET_BUCKET_SIZE && prefixBits < MAX_PREFIX_BITS)
      ++prefixBits;

    bucketStart.assign((1ULL << prefixBits) + 1, 0);

    size_t bucket = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
      const size_t target = prefixOf(keys[i]);

      while (bucket <= target)
        bucketStart[bucket++] = static_cast<uint32_t>(i);
    }

    while (bucket <= (1ULL << prefixBits))
      bucketStart[bucket++] = static_cast<uint32_t>(keys.size());
  }

  [[nodiscard]] size_t find(uint64_t key) const {
    const size_t bucket = prefixOf(key);

    const auto begin = keys.begin() + bucketStart[bucket];
    const auto end = keys.begin() + bucketStart[bucket + 1];

    const auto it = std::lower_bound(begin, end, key);

    if (it == end || *it != key)
      return NOT_FOUND;

    return static_cast<size_t>(it - keys.begin());
  }

  [[nodiscard]] size_t size() const {
    return keys.size();
  }

  [[nodiscard]] uint64_t keyAt(size_t i) const {
    return keys[i];
  }

  [[nodiscard]] NodeClassification classification(size_t i) const {
    return static_cast<NodeClassification>(flags[i] & CLASS_MASK);
  }

  [[nodiscard]] bool isVisited(size_t i) const {
    return (flags[i] & VISITED_BIT) != 0;
  }

  void markVisited(size_t i) {
    flags[i] |= VISITED_BIT;
  }

  /*
   * The slot holds the node's offset into the aggregate until the node has been
   * visited, and its offset into the .otb afterwards. Nothing reads the
   * aggregate offset once the node is claimed, which is why the two can share.
   */
  [[nodiscard]] uint64_t getAggOffset(size_t i) const {
    assert(!isVisited(i) && "aggregate offset is gone once the node is claimed");
    return slots[i];
  }

  void setOtbOffset(size_t i, uint32_t offset) {
    assert(isVisited(i) && "node not claimed and setting otb offset");
    slots[i] = offset;
  }

  [[nodiscard]] uint32_t getOtbOffset(size_t i) const {
    assert(isVisited(i) && "node was never written");
    return static_cast<uint32_t>(slots[i]);
  }

private:
  static constexpr uint8_t CLASS_MASK = 0x03;
  static constexpr uint8_t VISITED_BIT = 0x04;

  static constexpr size_t TARGET_BUCKET_SIZE = 16;
  static constexpr unsigned MAX_PREFIX_BITS = 22;

  [[nodiscard]] size_t prefixOf(uint64_t key) const {
    return static_cast<size_t>(key >> (64 - prefixBits));
  }

  std::vector<uint64_t> keys;
  std::vector<uint64_t> slots;
  std::vector<uint8_t> flags;

  std::vector<uint32_t> bucketStart;
  unsigned prefixBits = 1;
};

struct NodeBlobBuffer {
  alignas(NodeBlobHeader) std::byte buffer[maxBlobSize]{};
};

// one child move of a node, resolved against the index before we descend
struct ChildRef {
  uint16_t move;
  size_t indexPos; // PositionIndex::NOT_FOUND when the child is DROPPED
};

namespace {

std::vector<NodeBlobBuffer> blobBuffers;
std::vector<std::vector<ChildRef>> childBuffers;
std::vector<std::byte> readBuffer;

} // namespace

/*
 * MADV_WILLNEED queues the readahead and returns without blocking, so issuing a
 * run of these leaves several reads outstanding on the device at once. That is
 * the whole point: the aggregate lives on a USB-attached SSD whose random-read
 * throughput is ~4k IOPS at queue depth one but ~55k IOPS at queue depth 32.
 */
void prefetchRecords(const MappedArray<SpillRecord>& records, uint64_t recordIndex) {
  static const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));

  const auto* first = reinterpret_cast<const std::byte*>(records.data() + recordIndex);
  const auto* mapEnd = reinterpret_cast<const std::byte*>(records.data() + records.size());

  const uintptr_t start = reinterpret_cast<uintptr_t>(first) & ~(pageSize - 1);
  const size_t length =
      std::min<size_t>(PREFETCH_BYTES, reinterpret_cast<uintptr_t>(mapEnd) - start);

  madvise(reinterpret_cast<void*>(start), length, MADV_WILLNEED);
}

/*
 * Streams the aggregate once, summing each position's visits as it goes, and
 * records only the positions that clear DROP_LIMIT.
 */
int buildIndex(const char* fileName, PositionIndex& index) {
  int fd = open(fileName, O_RDONLY);
  if (fd == -1) {
    std::cerr << "open: " << std::strerror(errno) << '\n';
    return 1;
  }

  off_t fileSize = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  if (fileSize % sizeof(SpillRecord) != 0) [[unlikely]] {
    std::cerr << "file does not contain an exact amount of SpillRecords: " << fileSize << '\n';
    close(fd);
    throw std::runtime_error("bad file");
  }

  const size_t totalFileRecords = fileSize / sizeof(SpillRecord);

  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

  uint64_t runKey = 0;
  uint64_t runOffset = 0;
  uint64_t runVisits = 0;
  bool haveRun = false;

  size_t totalRecords = 0;

  auto finishRun = [&] {
    if (haveRun && runVisits >= DROP_LIMIT) {
      if (index.size() == MAX_ENTRIES) {
        std::cerr << "Too many kept positions; exceeded MAX_ENTRIES\n";
        throw std::runtime_error("index overflow");
      }

      index.append(runKey, runOffset, runVisits);
    }
  };

  while (true) {
    size_t bytesRead = 0;

    while (bytesRead < BUFFER_SIZE) {
      ssize_t n = read(fd, readBuffer.data() + bytesRead, BUFFER_SIZE - bytesRead);

      if (n == 0)
        break; // EOF

      if (n < 0) {
        if (errno == EINTR)
          continue;

        std::cerr << "read: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
      }

      bytesRead += static_cast<size_t>(n);
    }

    if (bytesRead == 0) {
      break;
    }

    const auto* records = reinterpret_cast<const SpillRecord*>(readBuffer.data());

    const size_t count = bytesRead / sizeof(SpillRecord);

    for (size_t i = 0; i < count; ++i) {
      const auto& record = records[i];

      if (!haveRun || record.zobrist != runKey) {
        finishRun();

        haveRun = true;
        runKey = record.zobrist;
        runOffset = totalRecords + i;
        runVisits = 0;
      }

      // classify only ever compares against DROP_LIMIT and LEAF_LIMIT, so the
      // sum is capped rather than carried to its true value
      if (runVisits < LEAF_LIMIT)
        runVisits += record.count;
    }

    totalRecords += count;

    if (totalRecords % 100'000'000 < count) {
      std::cout << (100.0 * totalRecords / totalFileRecords) << "%\n";
    }
  }

  finishRun();

  close(fd);

  index.finalize();

  return 0;
}

constexpr size_t PRINTING_DEPTH = 2;

void dfs(AppMetrics& metrics, std::ofstream& out, Stockfish::Position& pos,
         MappedArray<SpillRecord>& records, PositionIndex& index, size_t indexPos,
         size_t depth = 0) {
  metrics.maybePrint();

  assert(indexPos != PositionIndex::NOT_FOUND && "dropped nodes are never expanded");

  // we've already seen this one
  if (index.isVisited(indexPos))
    return;

  const auto myZobrist = pos.key();
  assert(index.keyAt(indexPos) == myZobrist && "index entry does not match the position");

  const auto classification = index.classification(indexPos);
  const uint64_t offset = index.getAggOffset(indexPos);

  // Claim the node before descending. Transpositions make the position graph
  // cyclic, and this is what stops a cycle from recursing forever.
  index.markVisited(indexPos);

  assert(depth < MAX_DEPTH);

  auto& outBuffer = blobBuffers[depth].buffer;
  std::memset(outBuffer, 0, maxBlobSize);
  NodeBlobHeader& node = *(new (&outBuffer) NodeBlobHeader());

  assert(node.nEntries == 0);

  SpillRecord catchall[static_cast<size_t>(Stockfish::RatingBucket::NUM_BUCKETS)]{};

  if (classification == NodeClassification::LEAF) {
    for (uint64_t i = offset; i < records.size(); ++i) {
      const auto& record = records[i];

      if (record.zobrist != myZobrist) {
        break;
      }

      if (record.move == Stockfish::Move::termination().raw()) {
        insertRecord(node, record);
        continue;
      }

      assert(record.bucket < static_cast<uint8_t>(Stockfish::RatingBucket::NUM_BUCKETS) &&
             "bucket out of range");

      auto& bucketRecord = catchall[record.bucket];
      bucketRecord.count += record.count;
      bucketRecord.draws += record.draws;
      bucketRecord.white_wins += record.white_wins;
    }
  } else {
    assert(classification == NodeClassification::FULL && "unexpected classification");

    Stockfish::StateInfo st;

    /*
     * Pass one: resolve every child against the index and queue the reads we
     * are about to need. Nothing here touches the disk synchronously -- the
     * lookups are pure RAM and the prefetches are asynchronous -- so by the
     * time pass two descends into the first child, the rest of the children are
     * already on their way in behind it.
     */
    auto& children = childBuffers[depth];
    children.clear();

    for (uint64_t i = offset; i < records.size(); ++i) {
      const auto& record = records[i];

      if (record.zobrist != myZobrist) {
        break;
      }

      const auto move = Stockfish::Move(record.move);

      if (move == Stockfish::Move::termination()) {
        continue;
      }

      assert(move != Stockfish::Move::forcedAggregation() && "unexpected move");

      // a move's buckets are adjacent, so this collapses them into one child
      if (!children.empty() && children.back().move == record.move) {
        continue;
      }

      pos.do_move(move, st);
      const size_t childPos = index.find(pos.key());
      pos.undo_move(move);

      children.push_back(ChildRef{.move = record.move, .indexPos = childPos});
      ++metrics.get<POSITIONS_CLASSIFIED>();

      if (childPos != PositionIndex::NOT_FOUND && !index.isVisited(childPos)) {
        prefetchRecords(records, index.getAggOffset(childPos));
        ++metrics.get<PREFETCHES_ISSUED>();
      }
    }

    // Pass two: walk the records again in order, so the blob is laid out
    // exactly as it always was.
    size_t childIdx = 0;

    for (uint64_t i = offset; i < records.size(); ++i) {
      const auto& record = records[i];

      if (record.zobrist != myZobrist) {
        break;
      }

      const auto move = Stockfish::Move(record.move);
      if (move == Stockfish::Move::termination()) {
        insertRecord(node, record);
        continue;
      }

      assert(childIdx < children.size() && "ran out of resolved children");

      const ChildRef child = children[childIdx++];
      assert(child.move == record.move && "child order diverged from record order");

      const auto childClass = child.indexPos == PositionIndex::NOT_FOUND
                                  ? NodeClassification::DROPPED
                                  : index.classification(child.indexPos);

      if (depth < PRINTING_DEPTH) {
        for (size_t d = 0; d < depth; ++d) {
          std::cout << "  ";
        }

        std::cout << Stockfish::UCIEngine::move(Stockfish::Move(move), false) << std::endl;
      }

      // a dropped child has no blob of its own; its statistics survive in the
      // parent's AGG pseudo-move below
      if (childClass != NodeClassification::DROPPED) {
        pos.do_move(move, st);
        dfs(metrics, out, pos, records, index, child.indexPos, depth + 1);
        pos.undo_move(move);
      }

      if (childClass == NodeClassification::DROPPED) {
        for (; i < records.size(); ++i) {
          const auto& childRecord = records[i];

          if (childRecord.zobrist != myZobrist || childRecord.move != record.move)
            break;

          assert(childRecord.bucket < static_cast<uint8_t>(Stockfish::RatingBucket::NUM_BUCKETS) &&
                 "bucket out of range");

          auto& bucketRecord = catchall[childRecord.bucket];
          bucketRecord.count += childRecord.count;
          bucketRecord.draws += childRecord.draws;
          bucketRecord.white_wins += childRecord.white_wins;
        }
      } else {
        for (; i < records.size(); ++i) {
          const auto& childRecord = records[i];

          if (childRecord.zobrist != myZobrist || childRecord.move != record.move)
            break;

          insertRecord(node, childRecord);
        }
      }
      // last move was a new move we have to go back as the outer loop will increment
      if (i < records.size()) {
        --i;
      }
    }

    assert(childIdx == children.size() && "not every resolved child was consumed");
  }

  for (uint8_t bucket = 0; bucket < static_cast<uint8_t>(Stockfish::RatingBucket::NUM_BUCKETS);
       ++bucket) {
    auto& record = catchall[bucket];

    if (record.count != 0) {
      record.move = Stockfish::Move::forcedAggregation().raw();
      record.bucket = bucket;

      insertRecord(node, record);
    }
  }

  assert(out.tellp() % 4 == 0);
  assert(out.tellp() / 4ULL <= (unsigned long long)std::numeric_limits<uint32_t>::max());
  uint32_t nodeOffsetDiv4 = out.tellp() / 4;

  index.setOtbOffset(indexPos, nodeOffsetDiv4);

  out.write(reinterpret_cast<const char*>(&node),
            sizeof(NodeBlobHeader) + node.nEntries * sizeof(NodeBlobEntry));
  ++metrics.get<POSITIONS_WRITTEN>();
  metrics.get<NODE_BLOBS_WRITTEN>() += node.nEntries;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: generate_otb <aggregate.bin> <output.otb>\n";
    return 1;
  }

  const char* outputFilename = argv[2];
  std::ofstream out(outputFilename, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("Failed to open " + std::string(outputFilename));

  OtbHeader header{};
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  Stockfish::Bitboards::init();
  Stockfish::Position::init();

  PositionIndex index;
  index.reserve(48'000'000);

  readBuffer.resize(BUFFER_BYTES);
  blobBuffers.resize(MAX_DEPTH);
  childBuffers.resize(MAX_DEPTH);

  std::cout << "Preloading index\n";

  if (int err = buildIndex(argv[1], index)) {
    std::cerr << "could not initialize the position index\n";
    return err;
  }

  // nothing reads the aggregate through this buffer again, and the page cache
  // wants the room more than we do
  readBuffer.clear();
  readBuffer.shrink_to_fit();

  std::cout << "Index loaded: " << index.size() << " positions above the drop limit\n";

  Stockfish::Position pos;
  Stockfish::StateInfo st;
  pos.set(Stockfish::StartFEN, false, &st);

  MappedArray<SpillRecord> records(argv[1], MmapAdvice::RANDOM);

  const size_t rootPos = index.find(pos.key());

  {
    const size_t indexSize = index.size();

    AppMetrics metrics(
        std::chrono::seconds(1),
        [indexSize](AppMetrics::Snapshot snapshot) {
          const auto written = snapshot.get<POSITIONS_WRITTEN>();

          // indexSize counts every position above the drop limit, including
          // those the pruned tree never reaches, so it is a ceiling on the node
          // count rather than a target
          std::cout << "positions written: " << written << " (<=" << indexSize << ")"
                    << " node blobs written: " << snapshot.get<NODE_BLOBS_WRITTEN>()
                    << " classifications: " << snapshot.get<POSITIONS_CLASSIFIED>()
                    << " prefetches: " << snapshot.get<PREFETCHES_ISSUED>() << "\n";
        },
        [](AppMetrics::Snapshot snapshot) {
          std::cout << "\n===== Final Metrics =====\n";
          std::cout << "Finished in: "
                    << std::chrono::duration_cast<std::chrono::seconds>(snapshot.getRuntime())
                           .count()
                    << " seconds\n";
          std::cout << "total children classified: " << snapshot.get<POSITIONS_CLASSIFIED>()
                    << "\n";
          std::cout << "total positions written: " << snapshot.get<POSITIONS_WRITTEN>() << "\n";
          std::cout << "node blobs written: " << snapshot.get<NODE_BLOBS_WRITTEN>() << "\n";
          std::cout << "prefetches issued: " << snapshot.get<PREFETCHES_ISSUED>() << "\n";
        });

    if (rootPos == PositionIndex::NOT_FOUND) {
      std::cerr << "start position is below the drop limit; nothing to expand\n";
    } else {
      dfs(metrics, out, pos, records, index, rootPos);
    }
  }

  std::cout << "writing index\n";

  uint64_t positions{};
  header.indexOffset = out.tellp();

  for (size_t i = 0; i < index.size(); ++i) {
    if (!index.isVisited(i))
      continue;

    assert((index.classification(i) == NodeClassification::LEAF ||
            index.classification(i) == NodeClassification::FULL) &&
           "unexpected classification");

    positions++;

    IndexEntry indexEntry{};
    indexEntry.zobrist = index.keyAt(i);
    indexEntry.offsetDiv4 = index.getOtbOffset(i);

    out.write(reinterpret_cast<const char*>(&indexEntry), sizeof(indexEntry));
  }

  header.nodes = positions;

  out.seekp(0);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  out.close();

  std::cout << "wrote " << header.nodes << " nodes, index offset at " << header.indexOffset << "\n";

  return 0;
}
