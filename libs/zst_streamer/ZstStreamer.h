#pragma once

#include <zstd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "metrics.h"

// multiple of
constexpr size_t CHUNK_SIZE = 1310720;

struct BYTES_READ : metrics::Counter<uint64_t> {};
struct BYTES_DECOMPRESSED : metrics::Counter<uint64_t> {};

class ZstStreamer {
public:
  using Metrics = metrics::DerivedMetrics<BYTES_READ, BYTES_DECOMPRESSED>;

  explicit ZstStreamer(Metrics appMetrics, const char* filename);
  ~ZstStreamer();

  bool readChunk(std::array<char, CHUNK_SIZE>& chunk, size_t& size);

private:
  Metrics& metrics;

  std::ifstream file_;

  ZSTD_DStream* zstd_;

  std::vector<char> compressed_;
  size_t compressedSize_;
  size_t compressedPos_;

  bool eof_;
};