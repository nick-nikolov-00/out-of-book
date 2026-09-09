#pragma once

#include "MappedFile.h"

#include <stdexcept>
#include <string>

template <typename T> class MappedArray {
public:
  MappedArray(const std::string& path, MmapAdvice advice = MmapAdvice::RANDOM) : file_(path, advice) {
    if (file_.size() % sizeof(T) != 0)
      throw std::runtime_error("Invalid file size");
  }

  void advice(MmapAdvice advice) {
    file_.advice(advice);
  }

  size_t size() const {
    return file_.size() / sizeof(T);
  }

  const T* data() const {
    return reinterpret_cast<const T*>(file_.data());
  }

  const T& operator[](size_t i) const {
    return data()[i];
  }

  const T* begin() const {
    return data();
  }

  const T* end() const {
    return data() + size();
  }

private:
  MappedFile file_;
};