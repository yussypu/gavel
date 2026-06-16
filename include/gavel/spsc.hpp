#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <vector>
#include "gavel/types.hpp"
#include "gavel/contracts.hpp"

namespace gavel {

// Single producer single consumer ring, power of two capacity; indices padded to kCacheLine (128 on M1; std constants report 64 there).
template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit SpscRing(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
    GAVEL_REQUIRE(capacity_pow2 >= 2 && (capacity_pow2 & mask_) == 0);
  }

  bool try_push(const T& v) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h - cached_tail_ > mask_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h - cached_tail_ > mask_) return false;
    }
    buf_[h & mask_] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t == cached_head_) return false;
    }
    out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return mask_ + 1; }

 private:
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  char pad0_[kCacheLine - sizeof(std::atomic<std::size_t>)];
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  char pad1_[kCacheLine - sizeof(std::atomic<std::size_t>)];
  alignas(kCacheLine) std::size_t cached_tail_{0};   // producer local
  char pad2_[kCacheLine - sizeof(std::size_t)];
  alignas(kCacheLine) std::size_t cached_head_{0};   // consumer local
  char pad3_[kCacheLine - sizeof(std::size_t)];
  const std::size_t mask_;
  std::vector<T> buf_;
};

}  // namespace gavel
