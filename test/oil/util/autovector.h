#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <vector>

namespace rocksdb {

template <class T, std::size_t kSize = 8>
class autovector {
 public:
  using value_type = T;
  using size_type = typename std::vector<T>::size_type;
  using reference = value_type&;
  using const_reference = const value_type&;

  autovector() = default;

  autovector(std::initializer_list<T> init) {
    for (const T& item : init) {
      push_back(item);
    }
  }

  bool only_in_stack() const {
    return vect_.capacity() == 0;
  }
  size_type size() const {
    return num_stack_items_ + vect_.size();
  }
  size_type capacity() const {
    return kSize + vect_.capacity();
  }

  void reserve(size_type cap) {
    if (cap > kSize) {
      vect_.reserve(cap - kSize);
    }
  }

  void push_back(const T& item) {
    if (num_stack_items_ < kSize) {
      inline_values_[num_stack_items_++] = item;
    } else {
      vect_.push_back(item);
    }
  }

  const_reference operator[](size_type n) const {
    assert(n < size());
    if (n < kSize) {
      return inline_values_[n];
    }
    return vect_[n - kSize];
  }

  reference operator[](size_type n) {
    assert(n < size());
    if (n < kSize) {
      return inline_values_[n];
    }
    return vect_[n - kSize];
  }

 private:
  size_type num_stack_items_ = 0;
  T inline_values_[kSize]{};
  std::vector<T> vect_;
};

}  // namespace rocksdb
