#pragma once

#include "common.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

class BoardDB {
public:
  struct NodeResult {
    alignas(NodeBlobHeader) std::byte data[maxBlobSize];
    std::optional<OteEntry> ote;

    NodeBlobHeader* header() {
      return reinterpret_cast<NodeBlobHeader*>(data);
    }

    bool hasOte() const {
      return ote.has_value();
    }
  };

  explicit BoardDB(const std::string& otbFilename);

  BoardDB(const std::string& otbFilename,
          const std::string& oteFilename);

  BoardDB(const BoardDB&) = delete;
  BoardDB& operator=(const BoardDB&) = delete;

  /*
   * `buffer` must point at caller-owned storage of at least
   * maxBlobSize bytes and suitable alignment for NodeBlobHeader.
   *
   * The node header and entries are written directly into `buffer`.
   *
   * If this BoardDB has an OTE file, result.ote is populated.
   */
  bool find(uint64_t zobrist,
            NodeResult& result) const;

  bool hasOte() const {
    return oteHeader_.has_value();
  }

private:
  mutable std::ifstream otb_;
  OtbHeader otbHeader_{};

  mutable std::ifstream ote_;
  std::optional<OteHeader> oteHeader_;

  bool findIndex(uint64_t zobrist, uint64_t& index) const;

  void readIndexEntry(uint64_t index, IndexEntry& entry) const;

  void readOteEntry(uint64_t index, OteEntry& entry) const;

  void readNode(uint64_t index,
                uint64_t zobrist,
                NodeBlobHeader& buffer) const;
};