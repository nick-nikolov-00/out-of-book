#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "unordered_dense.h"
#include "common.h"

constexpr size_t SHARDS = 64;
constexpr size_t WORKERS = 4;
constexpr size_t MAP_SIZE = 1ULL << 30; // ~1GB target

struct Key {
    uint64_t zobrist;
    uint16_t move;
    uint8_t bucket;

    bool operator==(const Key &o) const { return zobrist == o.zobrist && move == o.move && bucket == o.bucket; }
};


struct KeyHash {
    size_t operator()(const Key &k) const {
        uint64_t h = k.zobrist;
        h ^= uint64_t(k.move) << 32;
        h ^= uint64_t(k.bucket) << 48;

        h *= 0x9E3779B97F4A7C15ULL;

        return h ^ (h >> 32);
    }
};

static bool recordLess(const SpillRecord &a, const SpillRecord &b) {
    if (a.zobrist != b.zobrist)
        return a.zobrist < b.zobrist;

    if (a.move != b.move)
        return a.move < b.move;

    return a.bucket < b.bucket;
}

namespace fs = std::filesystem;

std::vector<fs::path> shardFiles(const fs::path& dir, int shard)
{
    std::vector<fs::path> files;

    const std::string prefix = "spill_" + std::to_string(shard) + "_";
    const std::string suffix = ".bin";

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;

        const std::string name = entry.path().filename().string();

        if (name.size() < prefix.size() + suffix.size())
            continue;

        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;

        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;

        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end()); // deterministic order

    return files;
}

static void aggregateShard(int shard, const std::string &shardDir, const std::string &outFile) {
    ankerl::unordered_dense::map<Key, SpillRecord, KeyHash> map;
    map.reserve(MAP_SIZE / sizeof(SpillRecord));

    std::vector<SpillRecord> sortBuffer;

    for (const auto& file : shardFiles(shardDir, shard)) {
        std::ifstream in(file, std::ios::binary);

        assert(in);

        constexpr size_t RECORDS_PER_READ = (2 * 1024 * 1024) / sizeof(SpillRecord);
        std::vector<SpillRecord> buffer(RECORDS_PER_READ);

        while (in) {
            in.read(reinterpret_cast<char *>(buffer.data()), buffer.size() * sizeof(SpillRecord));
            size_t n = in.gcount() / sizeof(SpillRecord);
            for (size_t i = 0; i < n; i++) {
                auto &r = buffer[i];

                Key k{r.zobrist, r.move, r.bucket};

                auto [it, inserted] = map.emplace(k, r);

                if (!inserted) {
                    it->second.count += r.count;
                    it->second.white_wins += r.white_wins;
                    it->second.draws += r.draws;
                }
            }
        }
    }

    sortBuffer.reserve(map.size());

    for (auto &[key, value]: map)
        sortBuffer.push_back(value);

    std::sort(sortBuffer.begin(), sortBuffer.end(), recordLess);
    std::ofstream out(outFile, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<char *>(sortBuffer.data()), sortBuffer.size() * sizeof(SpillRecord));
}


static void worker(int id, std::string shardDir, std::string outfile) {
    int start = id * (SHARDS / WORKERS);
    int end = start + SHARDS / WORKERS;

    for (int shard = start; shard < end; shard++) {
        std::cout << "worker " << id << " shard " << shard << "\n";

        aggregateShard(shard, shardDir, outfile);
    }
}


int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: merge_spills <spill_dir> <output>\n";
        return 1;
    }

    std::string dir = argv[1];
    std::string outFile = argv[2];

    std::vector<std::thread> workers;

    for (int i = 0; i < WORKERS; i++) {
        std::string customPath = outFile + "_w" + std::to_string(i);
        workers.emplace_back(worker, i, dir, customPath);
    }

    for (auto& worker : workers) {
        worker.join();
    }

    std::ofstream out(outFile, std::ios::binary);

    constexpr size_t BUFFER_SIZE = 1024 * 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    for (int i = 0; i < WORKERS; ++i) {
        std::cout<<"merging worker: " << i << "\n";

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
