#pragma once

#include "common.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/*
 * Read-only accessor for an .otb tree plus its optional .ote evaluations.
 *
 * Only the index is resident: the Zobrist keys and blob offsets are unpacked
 * into two parallel arrays, which costs less per node than the on-disk entry
 * and lets a binary search touch only the key array. Node blobs and eval
 * entries are pulled from the files with pread on every request, which keeps
 * the resident set to a fraction of the dataset and makes the store safe to
 * share across the server's threads.
 */
class OtbStore {
public:
  struct Node {
    uint16_t nEntries{};
    NodeBlobEntry entries[NodeBlobHeader::MAX_CHILDREN];
  };

  /* Pass an empty otePath to open the tree without evaluations. */
  OtbStore(const std::string& otbPath, const std::string& otePath);
  ~OtbStore();

  OtbStore(const OtbStore&) = delete;
  OtbStore& operator=(const OtbStore&) = delete;

  bool hasEvals() const {
    return oteFd_ != -1;
  }

  uint64_t nodeCount() const {
    return zobrists_.size();
  }

  /* Bytes of process memory held by the resident index. */
  uint64_t indexBytes() const {
    return zobrists_.size() * sizeof(uint64_t) + offsetsDiv4_.size() * sizeof(uint32_t);
  }

  std::optional<uint64_t> find(uint64_t zobrist) const;

  void readNode(uint64_t index, Node& out) const;

  /* Only valid when hasEvals(); entries are 1:1 with the OTB index order. */
  void readEvals(uint64_t index, OteEntry& out) const;

private:
  void loadIndex(uint64_t nodes, uint64_t indexOffset);

  void readExact(int fd, void* dst, size_t bytes, uint64_t offset, const char* what) const;

  int otbFd_ = -1;
  int oteFd_ = -1;

  std::vector<uint64_t> zobrists_;
  std::vector<uint32_t> offsetsDiv4_;
};
