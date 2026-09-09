#pragma once

#include <iostream>
#include <string>
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

enum class MmapAdvice { RANDOM, NORMAL, SEQUENTIAL };

class MappedFile {
public:
  explicit MappedFile(const std::string& path, MmapAdvice advice = MmapAdvice::RANDOM) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ == -1)
      throw std::runtime_error("open failed");

    struct stat st{};
    if (::fstat(fd_, &st) == -1) {
      ::close(fd_);
      throw std::runtime_error("fstat failed");
    }

    size_ = static_cast<size_t>(st.st_size);

    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) {
      ::close(fd_);
      throw std::runtime_error("mmap failed");
    }

    MappedFile::advice(advice);
  }

  ~MappedFile() {
    if (data_ != MAP_FAILED)
      ::munmap(data_, size_);

    if (fd_ != -1)
      ::close(fd_);
  }

  void advice(MmapAdvice advice) {
    if (advice == MmapAdvice::RANDOM) {
      ::madvise(data_, size_, MADV_RANDOM);
    } else if (advice == MmapAdvice::SEQUENTIAL) {
      ::madvise(data_, size_, MADV_SEQUENTIAL);
    } else if (advice == MmapAdvice::NORMAL) {
      ::madvise(data_, size_, MADV_NORMAL);
    } else {
      throw std::runtime_error("illegal arg");
    }
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const std::byte* data() const {
    return static_cast<const std::byte*>(data_);
  }

  size_t size() const {
    return size_;
  }

private:
  int fd_ = -1;
  void* data_ = MAP_FAILED;
  size_t size_ = 0;
};