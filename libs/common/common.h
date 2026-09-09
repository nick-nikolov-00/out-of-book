#pragma once
#include "types.h"

#include <cstdint>
#include <cstring>
#include <limits>

struct SpillRecord {
  // key
  uint64_t zobrist;
  uint16_t move;
  uint8_t bucket;
  uint8_t pad;

  // val
  uint32_t count;
  uint32_t white_wins;
  uint32_t draws;
};

namespace detail {
constexpr uint64_t FIRST_BIT = 0x80'00'00'00'00'00'00'00ULL;
}

struct OtbHeader {
  static constexpr uint16_t VERSION = 0;
  // randomly chosen
  static constexpr uint64_t MAGIC = 0x48'c6'd0'42'4d'50'5e'adULL;
  static_assert((MAGIC & detail::FIRST_BIT) == 0, "first bit in otb header magic must be unset");

  uint64_t magic = MAGIC;
  uint16_t version = VERSION;
  uint64_t nodes{};
  uint64_t indexOffset{};
  uint8_t reserved[32]{};
};

static_assert(sizeof(OtbHeader) == 64);

struct OteHeader {
  static constexpr uint16_t VERSION = 0;
  static constexpr uint64_t MAGIC = OtbHeader::MAGIC | detail::FIRST_BIT;
  static_assert((MAGIC & detail::FIRST_BIT) != 0, "first bit in ote header magic must be set");

  uint64_t magic = MAGIC;
  uint16_t version = VERSION;
  uint8_t reserved[48]{};
};

static_assert(sizeof(OteHeader) == 64);

struct OteEntry {
  uint16_t moveMax[Stockfish::NUMBER_OF_BUCKETS]{};

  float evalW[Stockfish::NUMBER_OF_BUCKETS]{};
  float evalB[Stockfish::NUMBER_OF_BUCKETS]{};
};
static_assert(sizeof(OteEntry) == 92, "bump version?");

struct NodeBlobEntry {
  uint16_t move{};
  uint8_t bucket{};
  uint8_t pad{};
  uint32_t count{};
  uint32_t white_wins{};
  uint32_t draws{};
};

struct NodeBlobHeader {
  static constexpr size_t MAX_CHILDREN = 1000;

  uint16_t nEntries{};
  uint8_t pad[2]{};
  NodeBlobEntry entries[];
};

constexpr size_t maxBlobSize = sizeof(NodeBlobHeader) + sizeof(NodeBlobEntry) * NodeBlobHeader::MAX_CHILDREN;
static_assert(NodeBlobHeader::MAX_CHILDREN <= std::numeric_limits<decltype(NodeBlobHeader::nEntries)>::max(), "max children doesn't fit");

struct IndexEntry {
  uint64_t zobrist{};
  uint32_t offsetDiv4{};
};