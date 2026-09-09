#include "ZstStreamer.h"

#include <cassert>
#include <iostream>

#include "metrics.h"

ZstStreamer::ZstStreamer(Metrics metrics, const char* filename)
    : metrics(metrics), file_(filename, std::ios::binary), zstd_(ZSTD_createDStream()),
      compressed_(ZSTD_DStreamInSize()), compressedSize_(0), compressedPos_(0), eof_(false) {
  assert(CHUNK_SIZE % ZSTD_DStreamOutSize() == 0 &&
         "CHUNK_SIZE must be a multiple of ZSTD_DStreamOutSize()");

  if (!file_)
    throw std::runtime_error("cannot open file");

  size_t r = ZSTD_initDStream(zstd_);

  if (ZSTD_isError(r))
    throw std::runtime_error(ZSTD_getErrorName(r));
}

ZstStreamer::~ZstStreamer() {
  ZSTD_freeDStream(zstd_);
}

bool ZstStreamer::readChunk(std::array<char, CHUNK_SIZE>& chunk, size_t& size) {
  size = 0;

  // FIXME: should this go up to CHUNK_SIZE? do we null terminate?
  while (size < CHUNK_SIZE - 1) {
    if (compressedPos_ == compressedSize_) {
      file_.read(compressed_.data(), compressed_.size());

      compressedSize_ = file_.gcount();
      compressedPos_ = 0;
      metrics.get<BYTES_READ>() += compressedSize_;

      if (compressedSize_ == 0)
        break;
    }

    ZSTD_inBuffer input{compressed_.data(), compressedSize_, compressedPos_};

    ZSTD_outBuffer output{chunk.data() + size, CHUNK_SIZE - size, 0};

    size_t ret = ZSTD_decompressStream(zstd_, &output, &input);

    if (ZSTD_isError(ret)) {
      throw std::runtime_error(ZSTD_getErrorName(ret));
    }

    compressedPos_ = input.pos;
    size += output.pos;
    metrics.get<BYTES_DECOMPRESSED>() += output.pos;
  }

  return size != 0;
}
