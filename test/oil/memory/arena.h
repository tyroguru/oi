#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <memory>

namespace rocksdb {

class Logger {};

class AllocTracker {
 public:
  void Allocate(std::size_t bytes) {
    allocated_ += bytes;
  }

  void FreeMem() {
    allocated_ = 0;
  }

  bool is_freed() const {
    return allocated_ == 0;
  }

 private:
  std::size_t allocated_ = 0;
};

class Allocator {
 public:
  virtual ~Allocator() = default;
  virtual char* Allocate(std::size_t bytes) = 0;
  virtual char* AllocateAligned(std::size_t bytes,
                                std::size_t huge_page_size = 0,
                                Logger* logger = nullptr) = 0;
  virtual std::size_t BlockSize() const = 0;
};

class MemMapping {
 public:
  static constexpr bool kHugePageSupported = false;

  void* Get() const {
    return addr_;
  }

  std::size_t Length() const {
    return length_;
  }

 private:
  void* addr_ = nullptr;
  std::size_t length_ = 0;
};

class Arena : public Allocator {
 public:
  Arena(const Arena&) = delete;
  void operator=(const Arena&) = delete;

  static constexpr std::size_t kInlineSize = 2048;
  static constexpr std::size_t kMinBlockSize = 4096;
  static constexpr std::size_t kMaxBlockSize = 2u << 30;
  static constexpr unsigned kAlignUnit = alignof(std::max_align_t);

  explicit Arena(std::size_t block_size = kMinBlockSize,
                 AllocTracker* tracker = nullptr,
                 std::size_t huge_page_size = 0)
      : kBlockSize(OptimizeBlockSize(block_size)), tracker_(tracker) {
    (void)huge_page_size;
    alloc_bytes_remaining_ = sizeof(inline_block_);
    blocks_memory_ += alloc_bytes_remaining_;
    aligned_alloc_ptr_ = inline_block_;
    unaligned_alloc_ptr_ = inline_block_ + alloc_bytes_remaining_;
    if (tracker_ != nullptr) {
      tracker_->Allocate(kInlineSize);
    }
  }

  ~Arena() override {
    if (tracker_ != nullptr) {
      tracker_->FreeMem();
    }
  }

  char* Allocate(std::size_t bytes) override {
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_) {
      unaligned_alloc_ptr_ -= bytes;
      alloc_bytes_remaining_ -= bytes;
      return unaligned_alloc_ptr_;
    }
    return AllocateFallback(bytes, false);
  }

  char* AllocateAligned(std::size_t bytes,
                        std::size_t huge_page_size = 0,
                        Logger* logger = nullptr) override {
    (void)huge_page_size;
    (void)logger;
    assert(bytes > 0);

    const std::size_t current_mod =
        reinterpret_cast<uintptr_t>(aligned_alloc_ptr_) & (kAlignUnit - 1);
    const std::size_t slop = current_mod == 0 ? 0 : kAlignUnit - current_mod;
    const std::size_t needed = bytes + slop;
    if (needed <= alloc_bytes_remaining_) {
      char* result = aligned_alloc_ptr_ + slop;
      aligned_alloc_ptr_ += needed;
      alloc_bytes_remaining_ -= needed;
      return result;
    }

    return AllocateFallback(bytes, true);
  }

  std::size_t ApproximateMemoryUsage() const {
    return blocks_memory_ + blocks_.size() * sizeof(char*) -
           alloc_bytes_remaining_;
  }

  std::size_t MemoryAllocatedBytes() const {
    return blocks_memory_;
  }

  std::size_t AllocatedAndUnused() const {
    return alloc_bytes_remaining_;
  }

  std::size_t IrregularBlockNum() const {
    return irregular_block_num;
  }

  std::size_t BlockSize() const override {
    return kBlockSize;
  }

  bool IsInInlineBlock() const {
    return blocks_.empty() && huge_blocks_.empty();
  }

  static std::size_t OptimizeBlockSize(std::size_t block_size) {
    block_size = std::max(kMinBlockSize, block_size);
    block_size = std::min(kMaxBlockSize, block_size);
    if (block_size % kAlignUnit != 0) {
      block_size = (1 + block_size / kAlignUnit) * kAlignUnit;
    }
    return block_size;
  }

 private:
  alignas(std::max_align_t) char inline_block_[kInlineSize];
  const std::size_t kBlockSize;
  std::deque<std::unique_ptr<char[]>> blocks_;
  std::deque<MemMapping> huge_blocks_;
  std::size_t irregular_block_num = 0;
  char* unaligned_alloc_ptr_ = nullptr;
  char* aligned_alloc_ptr_ = nullptr;
  std::size_t alloc_bytes_remaining_ = 0;
  std::size_t hugetlb_size_ = 0;

  char* AllocateFallback(std::size_t bytes, bool aligned) {
    if (bytes > kBlockSize / 4) {
      ++irregular_block_num;
      return AllocateNewBlock(bytes);
    }

    const std::size_t size = kBlockSize;
    char* block_head = AllocateNewBlock(size);
    alloc_bytes_remaining_ = size - bytes;

    if (aligned) {
      aligned_alloc_ptr_ = block_head + bytes;
      unaligned_alloc_ptr_ = block_head + size;
      return block_head;
    }

    aligned_alloc_ptr_ = block_head;
    unaligned_alloc_ptr_ = block_head + size - bytes;
    return unaligned_alloc_ptr_;
  }

  char* AllocateNewBlock(std::size_t block_bytes) {
    char* block = new char[block_bytes];
    blocks_.push_back(std::unique_ptr<char[]>(block));
    blocks_memory_ += block_bytes;
    if (tracker_ != nullptr) {
      tracker_->Allocate(block_bytes);
    }
    return block;
  }

  std::size_t blocks_memory_ = 0;
  AllocTracker* tracker_;
};

}  // namespace rocksdb
