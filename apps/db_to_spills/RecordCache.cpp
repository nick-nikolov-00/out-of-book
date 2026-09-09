#include "RecordCache.h"

#include <cstring>
#include <filesystem>
#include <iostream>

#include "AppMetrics.h"
#include "metrics.h"

namespace {
constexpr bool DEBUG_DISABLE_WRITES = false;

__always_inline uint64_t hashKey(uint64_t zobrist, uint16_t move, uint8_t bucket) {
  return zobrist;
  uint64_t h = zobrist ^ (uint64_t(move) << 16) ^ uint64_t(bucket);

  h *= 0x9E3779B97F4A7C15ULL;
  return h ^ (h >> 32);
}
} // namespace

RecordCache::RecordCache(AppMetrics& appMetrics, int workerId, const char* spillDir)
    : appMetrics(appMetrics),
      writeBuffers(std::make_unique<uint8_t[]>(SHARDS * WRITE_BUFFER_SIZE)) {
  map = std::make_unique<SpillRecord[]>(SLOTS);
  std::memset(map.get(), 0, SLOTS * sizeof(SpillRecord));

  if (DEBUG_DISABLE_WRITES)
    return;

  namespace fs = std::filesystem;

  fs::create_directories(spillDir);

  for (size_t s = 0; s < SHARDS; s++) {
    fs::path p = fs::path(spillDir) /
                 ("spill_" + std::to_string(s) + "_" + std::to_string(workerId) + ".bin");

    spillFiles[s].open(p, std::ios::binary | std::ios::app);
    if (!spillFiles[s]) {
      std::cerr << "Failed to open " << p << '\n';
      std::abort();
    }
  }
}

RecordCache::~RecordCache() {
  for (size_t i = 0; i < SLOTS; ++i) {
    const SpillRecord& rec = map[i];

    if (rec.zobrist == 0)
      continue;

    uint32_t shard = rec.zobrist >> (64 - SHARD_BITS);

    uint8_t* buffer = writeBuffers.get() + shard * WRITE_BUFFER_SIZE;
    std::memcpy(buffer + writePos[shard], &rec, sizeof(SpillRecord));

    writePos[shard] += sizeof(SpillRecord);

    if (writePos[shard] == WRITE_BUFFER_SIZE)
      flushBuffer(shard);
  }

  if (DEBUG_DISABLE_WRITES)
    return;

  for (uint32_t shard = 0; shard < SHARDS; ++shard) {
    if (writePos[shard] != 0)
      flushBuffer(shard);

    spillFiles[shard].close();
  }
}

void RecordCache::add(uint64_t zobrist, uint16_t move, Stockfish::RatingBucket bucket,
                      Stockfish::GameResult result) {
  SpillRecord incoming{};
  incoming.zobrist = zobrist;
  incoming.move = move;
  incoming.bucket = static_cast<uint8_t>(bucket);
  incoming.pad = 0;
  incoming.count = 1;

  switch (result) {
    case Stockfish::GameResult::WhiteWin:
      incoming.white_wins = 1;
      incoming.draws = 0;
      break;

    case Stockfish::GameResult::Draw:
      incoming.white_wins = 0;
      incoming.draws = 1;
      break;

    default:
      incoming.white_wins = 0;
      incoming.draws = 0;
      break;
  }

  size_t mask = SLOTS - 1;
  // size_t pos = hashKey(zobrist, move, incoming.bucket) & mask;
  size_t pos = zobrist & mask;

  size_t smallestPos = pos;
  uint32_t smallestCount = UINT32_MAX;

  for (size_t probe = 0; probe < MAX_RUN; probe++) {
    SpillRecord& slot = map[pos];

    // empty
    if (slot.zobrist == 0) {
      slot = incoming;
      return;
    }

    // identical key (first 12 bytes)
    if (slot.zobrist == zobrist && slot.move == incoming.move && slot.bucket == incoming.bucket) {
      slot.count++;

      slot.white_wins += incoming.white_wins;
      slot.draws += incoming.draws;

      return;
    }

    if (slot.count < smallestCount) {
      smallestCount = slot.count;
      smallestPos = pos;
    }

    pos = (pos + 1) & mask;
  }

  // Evict least-frequent entry in the run.
  SpillRecord evicted = map[smallestPos];
  map[smallestPos] = incoming;

  uint32_t shard = evicted.zobrist >> (64 - SHARD_BITS);

  uint8_t* buffer = writeBuffers.get() + shard * WRITE_BUFFER_SIZE;
  std::memcpy(buffer + writePos[shard], &evicted, sizeof(SpillRecord));

  writePos[shard] += sizeof(SpillRecord);

  if (writePos[shard] == WRITE_BUFFER_SIZE)
    flushBuffer(shard);
}

void RecordCache::flushBuffer(uint32_t shard) {
  appMetrics.get<BYTES_WRITTEN>() += writePos[shard];
  uint8_t* buffer = writeBuffers.get() + shard * WRITE_BUFFER_SIZE;
  if (!DEBUG_DISABLE_WRITES)
    spillFiles[shard].write(reinterpret_cast<const char*>(buffer), writePos[shard]);

  writePos[shard] = 0;
}
