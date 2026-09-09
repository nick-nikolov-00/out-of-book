#include "BoardDB.h"

#include <limits>
#include <stdexcept>

BoardDB::BoardDB(const std::string& otbFilename)
    : otb_(otbFilename, std::ios::binary) {

  if (!otb_)
    throw std::runtime_error("cannot open OTB file: " + otbFilename);

  otb_.read(reinterpret_cast<char*>(&otbHeader_), sizeof(otbHeader_));

  if (!otb_)
    throw std::runtime_error("cannot read OTB header");

  if (otbHeader_.magic != OtbHeader::MAGIC)
    throw std::runtime_error("invalid OTB magic");

  if (otbHeader_.version != OtbHeader::VERSION) {
    throw std::runtime_error(
        "unsupported OTB version: " +
        std::to_string(otbHeader_.version));
  }
}

BoardDB::BoardDB(const std::string& otbFilename,
                 const std::string& oteFilename)
    : BoardDB(otbFilename) {

  ote_.open(oteFilename, std::ios::binary);

  if (!ote_)
    throw std::runtime_error("cannot open OTE file: " + oteFilename);

  OteHeader header{};

  ote_.read(reinterpret_cast<char*>(&header), sizeof(header));

  if (!ote_)
    throw std::runtime_error("cannot read OTE header");

  if (header.magic != OteHeader::MAGIC)
    throw std::runtime_error("invalid OTE magic");

  if (header.version != OteHeader::VERSION) {
    throw std::runtime_error(
        "unsupported OTE version: " +
        std::to_string(header.version));
  }

  oteHeader_ = header;
}

bool BoardDB::find(uint64_t zobrist, NodeResult& result) const {
  result.ote.reset();

  uint64_t index = 0;

  if (!findIndex(zobrist, index))
    return false;

  readNode(index, zobrist, *result.header());

  if (oteHeader_) {
    OteEntry entry{};
    readOteEntry(index, entry);
    result.ote = entry;
  }

  return true;
}

bool BoardDB::findIndex(uint64_t zobrist, uint64_t& index) const {
  uint64_t lo = 0;
  uint64_t hi = otbHeader_.nodes;

  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;

    IndexEntry entry{};
    readIndexEntry(mid, entry);

    if (entry.zobrist < zobrist)
      lo = mid + 1;
    else
      hi = mid;
  }

  if (lo >= otbHeader_.nodes)
    return false;

  IndexEntry entry{};
  readIndexEntry(lo, entry);

  if (entry.zobrist != zobrist)
    return false;

  index = lo;
  return true;
}

void BoardDB::readIndexEntry(uint64_t index,
                             IndexEntry& entry) const {
  const uint64_t offset =
      otbHeader_.indexOffset +
      index * sizeof(IndexEntry);

  otb_.clear();
  otb_.seekg(static_cast<std::streamoff>(offset));

  if (!otb_)
    throw std::runtime_error(
        "failed to seek to OTB index entry");

  otb_.read(
      reinterpret_cast<char*>(&entry),
      sizeof(entry));

  if (!otb_)
    throw std::runtime_error(
        "failed to read OTB index entry");
}

void BoardDB::readOteEntry(uint64_t index,
                           OteEntry& entry) const {
  const uint64_t offset =
      sizeof(OteHeader) +
      index * sizeof(OteEntry);

  ote_.clear();
  ote_.seekg(static_cast<std::streamoff>(offset));

  if (!ote_)
    throw std::runtime_error(
        "failed to seek to OTE entry");

  ote_.read(
      reinterpret_cast<char*>(&entry),
      sizeof(entry));

  if (!ote_)
    throw std::runtime_error(
        "failed to read OTE entry");
}

void BoardDB::readNode(uint64_t index,
                       uint64_t zobrist,
                       NodeBlobHeader& buffer) const {
  IndexEntry indexEntry{};
  readIndexEntry(index, indexEntry);

  if (indexEntry.zobrist != zobrist) {
    throw std::runtime_error(
        "OTB index Zobrist does not match requested Zobrist");
  }

  /*
   * offsetDiv4 stores the node blob offset divided by four.
   */
  const uint64_t blobOffset =
      static_cast<uint64_t>(indexEntry.offsetDiv4) * 4;

  otb_.clear();
  otb_.seekg(static_cast<std::streamoff>(blobOffset));

  if (!otb_)
    throw std::runtime_error(
        "failed to seek to OTB node blob");

  /*
   * NodeBlobHeader contains the flexible array member, so sizeof()
   * only covers the fixed header portion.
   */
  otb_.read(
      reinterpret_cast<char*>(&buffer),
      sizeof(NodeBlobHeader));

  if (!otb_)
    throw std::runtime_error(
        "failed to read OTB node blob header");

  if (buffer.nEntries > NodeBlobHeader::MAX_CHILDREN) {
    throw std::runtime_error(
        "invalid OTB node blob: too many entries");
  }

  const uint64_t blobSize =
      sizeof(NodeBlobHeader) +
      static_cast<uint64_t>(buffer.nEntries) *
          sizeof(NodeBlobEntry);

  if (blobSize > maxBlobSize) {
    throw std::runtime_error(
        "invalid OTB node blob size");
  }

  if (buffer.nEntries != 0) {
    otb_.read(
        reinterpret_cast<char*>(buffer.entries),
        static_cast<std::streamsize>(
            static_cast<uint64_t>(buffer.nEntries) *
            sizeof(NodeBlobEntry)));

    if (!otb_)
      throw std::runtime_error(
          "failed to read OTB node blob entries");
  }
}