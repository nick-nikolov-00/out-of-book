#include "OtbStore.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr size_t INDEX_CHUNK_ENTRIES = 1u << 20; // 16 MiB of IndexEntry per read

int openRead(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);

  if (fd == -1)
    throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));

  return fd;
}

} // namespace

OtbStore::OtbStore(const std::string& otbPath, const std::string& otePath) {
  otbFd_ = openRead(otbPath);

  OtbHeader otb{};
  readExact(otbFd_, &otb, sizeof(otb), 0, "OTB header");

  if (otb.magic != OtbHeader::MAGIC)
    throw std::runtime_error("not an OTB file: " + otbPath);

  if (otb.version != OtbHeader::VERSION)
    throw std::runtime_error("unsupported OTB version: " + std::to_string(otb.version));

  loadIndex(otb.nodes, otb.indexOffset);

  if (otePath.empty())
    return;

  oteFd_ = openRead(otePath);

  OteHeader ote{};
  readExact(oteFd_, &ote, sizeof(ote), 0, "OTE header");

  if (ote.magic != OteHeader::MAGIC)
    throw std::runtime_error("not an OTE file: " + otePath);

  if (ote.version != OteHeader::VERSION)
    throw std::runtime_error("unsupported OTE version: " + std::to_string(ote.version));

  /*
   * The OTE file is a flat array parallel to the OTB index, so a size
   * mismatch means the two files came from different runs and every eval
   * would silently belong to the wrong position.
   */
  const off_t oteSize = ::lseek(oteFd_, 0, SEEK_END);
  const uint64_t expected = sizeof(OteHeader) + otb.nodes * sizeof(OteEntry);

  if (oteSize < 0 || static_cast<uint64_t>(oteSize) != expected) {
    throw std::runtime_error("OTE size does not match OTB node count; expected " +
                             std::to_string(expected) + " bytes, got " + std::to_string(oteSize));
  }
}

OtbStore::~OtbStore() {
  if (otbFd_ != -1)
    ::close(otbFd_);

  if (oteFd_ != -1)
    ::close(oteFd_);
}

void OtbStore::loadIndex(uint64_t nodes, uint64_t indexOffset) {
  zobrists_.resize(nodes);
  offsetsDiv4_.resize(nodes);

  std::vector<IndexEntry> chunk(INDEX_CHUNK_ENTRIES);

  for (uint64_t start = 0; start < nodes; start += INDEX_CHUNK_ENTRIES) {
    const uint64_t n = std::min<uint64_t>(INDEX_CHUNK_ENTRIES, nodes - start);

    readExact(otbFd_, chunk.data(), n * sizeof(IndexEntry),
              indexOffset + start * sizeof(IndexEntry), "OTB index");

    for (uint64_t i = 0; i < n; ++i) {
      zobrists_[start + i] = chunk[i].zobrist;
      offsetsDiv4_[start + i] = chunk[i].offsetDiv4;
    }
  }

  /* find() binary searches, so a non-sorted index would fail silently. */
  if (!std::is_sorted(zobrists_.begin(), zobrists_.end()))
    throw std::runtime_error("OTB index is not sorted by Zobrist key");
}

std::optional<uint64_t> OtbStore::find(uint64_t zobrist) const {
  const auto it = std::lower_bound(zobrists_.begin(), zobrists_.end(), zobrist);

  if (it == zobrists_.end() || *it != zobrist)
    return std::nullopt;

  return static_cast<uint64_t>(it - zobrists_.begin());
}

void OtbStore::readNode(uint64_t index, Node& out) const {
  const uint64_t offset = static_cast<uint64_t>(offsetsDiv4_[index]) * 4;

  /* NodeBlobHeader ends in a flexible array member, so it cannot be declared
   * as a local; read its fixed prefix into raw storage instead. */
  std::byte header[sizeof(NodeBlobHeader)];
  readExact(otbFd_, header, sizeof(header), offset, "node blob header");

  uint16_t nEntries = 0;
  std::memcpy(&nEntries, header, sizeof(nEntries));

  if (nEntries > NodeBlobHeader::MAX_CHILDREN)
    throw std::runtime_error("node blob claims too many entries");

  out.nEntries = nEntries;

  if (out.nEntries != 0) {
    readExact(otbFd_, out.entries, out.nEntries * sizeof(NodeBlobEntry),
              offset + sizeof(NodeBlobHeader), "node blob entries");
  }
}

void OtbStore::readEvals(uint64_t index, OteEntry& out) const {
  readExact(oteFd_, &out, sizeof(out), sizeof(OteHeader) + index * sizeof(OteEntry), "OTE entry");
}

void OtbStore::readExact(int fd, void* dst, size_t bytes, uint64_t offset, const char* what) const {
  auto* p = static_cast<std::byte*>(dst);

  while (bytes > 0) {
    const ssize_t n = ::pread(fd, p, bytes, static_cast<off_t>(offset));

    if (n < 0) {
      if (errno == EINTR)
        continue;

      throw std::runtime_error(std::string("failed to read ") + what + ": " + std::strerror(errno));
    }

    if (n == 0)
      throw std::runtime_error(std::string("unexpected end of file reading ") + what);

    p += n;
    bytes -= static_cast<size_t>(n);
    offset += static_cast<uint64_t>(n);
  }
}
