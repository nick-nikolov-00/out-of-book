#pragma once

#include "MappedFile.h"
#include "common.h"
#include "types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace lineodds {

/* The .otb mapped for reading, with its index left in place rather than
 * copied. A node's blob holds one row per (move, rating band). */
class Book {
 public:
  explicit Book(const std::string& path);

  std::span<const NodeBlobEntry> at(uint64_t zobrist) const;

 private:
  std::optional<MappedFile> file;
  const std::byte* data = nullptr;
  std::span<const IndexEntry> index;
};

} // namespace lineodds
