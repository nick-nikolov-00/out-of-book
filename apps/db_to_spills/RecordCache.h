#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "AppMetrics.h"
#include "types.h"
#include "common.h"

constexpr size_t SLOTS = 67108864; // power of two
constexpr size_t WRITE_BUFFER_SIZE = 999984;
constexpr size_t SHARD_BITS = 6;
constexpr size_t SHARDS = 1 << SHARD_BITS;
constexpr size_t MAX_RUN = 16;

static_assert(SHARDS == 64);
static_assert(WRITE_BUFFER_SIZE % sizeof(SpillRecord) == 0);

class RecordCache {
public:
    // records in spillDir/spill_[SHARD]_workerId.recspill
    RecordCache (AppMetrics& appMetrics, int workerId, const char* spillDir);
    ~RecordCache();

    void add(uint64_t zobrist, uint16_t move, Stockfish::RatingBucket bucket, Stockfish::GameResult result);

private:
    void flushBuffer(uint32_t shard);

    AppMetrics& appMetrics;

    std::unique_ptr<SpillRecord[]> map;

    std::unique_ptr<uint8_t[]> writeBuffers;
    size_t writePos[SHARDS] = {};

    std::array<std::ofstream, SHARDS> spillFiles;
};

