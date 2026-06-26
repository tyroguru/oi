#pragma once

#include <cassert>
#include <cstddef>
#include <memory>

namespace rocksdb {

template <typename T>
class CoreLocalArray {
 public:
  CoreLocalArray() : data_(new T[kSize]), size_shift_(kSizeShift) {
  }

  std::size_t Size() const {
    return std::size_t{1} << size_shift_;
  }

  T* AccessAtCore(std::size_t core_idx) const {
    assert(core_idx < Size());
    return &data_[core_idx];
  }

 private:
  static constexpr int kSizeShift = 2;
  static constexpr std::size_t kSize = std::size_t{1} << kSizeShift;

  std::unique_ptr<T[]> data_;
  int size_shift_;
};

}  // namespace rocksdb
