#include "Book.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace lineodds {

namespace {

struct ZobristCompare {
  bool operator()(const IndexEntry& e, uint64_t key) const { return e.zobrist < key; }
  bool operator()(uint64_t key, const IndexEntry& e) const { return key < e.zobrist; }
};

} // namespace

Book::Book(const std::string& path) {
  file.emplace(path, MmapAdvice::RANDOM);
  data = file->data();

  if (file->size() < sizeof(OtbHeader))
    throw std::runtime_error("file is too small to hold an OTB header");

  OtbHeader header{};
  std::memcpy(&header, data, sizeof(header));

  if (header.magic != OtbHeader::MAGIC)
    throw std::runtime_error("invalid magic in " + path);
  if (header.version != OtbHeader::VERSION)
    throw std::runtime_error("invalid version in " + path);
  if (header.indexOffset > file->size() ||
      (file->size() - header.indexOffset) / sizeof(IndexEntry) != header.nodes)
    throw std::runtime_error("OTB index does not match the node count in the header");

  const std::byte* start = data + header.indexOffset;
  assert(reinterpret_cast<uintptr_t>(start) % alignof(IndexEntry) == 0);

  index = {reinterpret_cast<const IndexEntry*>(start), header.nodes};
}

std::span<const NodeBlobEntry> Book::at(uint64_t zobrist) const {
  const auto it = std::lower_bound(index.begin(), index.end(), zobrist, ZobristCompare{});

  if (it == index.end() || it->zobrist != zobrist)
    return {};

  const uint64_t offset = static_cast<uint64_t>(it->offsetDiv4) * 4ULL;
  const auto& blob = *reinterpret_cast<const NodeBlobHeader*>(data + offset);

  return {blob.entries, blob.nEntries};
}

} // namespace lineodds
