#pragma once
#include <cstdint>
#include <vector>
#include "gavel/hash.hpp"

namespace gavel {

// Open addressing map OrderId -> pool index, linear probing, power of two capacity; iteration order never reaches engine output.
class IdMap {
 public:
  static constexpr std::uint32_t kEmpty = 0xffffffffu;

  explicit IdMap(std::uint32_t initial_pow2 = 16) { rehash(1u << initial_pow2); }

  void insert(std::uint64_t key, std::uint32_t value) {
    if ((size_ + 1) * 10 >= cap_ * 7) rehash(cap_ * 2);
    std::size_t i = slot(key);
    while (vals_[i] != kEmpty) {
      if (keys_[i] == key) { vals_[i] = value; return; }
      i = (i + 1) & mask_;
    }
    keys_[i] = key; vals_[i] = value; ++size_;
  }

  std::uint32_t find(std::uint64_t key) const {
    std::size_t i = slot(key);
    while (vals_[i] != kEmpty) {
      if (keys_[i] == key) return vals_[i];
      i = (i + 1) & mask_;
    }
    return kEmpty;
  }

  void erase(std::uint64_t key) {
    std::size_t i = slot(key);
    while (vals_[i] != kEmpty) {
      if (keys_[i] == key) {
        // Backward shift deletion keeps probe chains intact.
        std::size_t j = i;
        for (;;) {
          vals_[j] = kEmpty;
          std::size_t k = j;
          for (;;) {
            k = (k + 1) & mask_;
            if (vals_[k] == kEmpty) { --size_; return; }
            const std::size_t home = slot(keys_[k]);
            const bool movable = (j <= k) ? (home <= j || home > k) : (home <= j && home > k);
            if (movable) break;
          }
          keys_[j] = keys_[k]; vals_[j] = vals_[k];
          j = k;
        }
      }
      i = (i + 1) & mask_;
    }
  }

  std::size_t size() const { return size_; }

 private:
  std::size_t slot(std::uint64_t key) const { return mix64(key) & mask_; }

  void rehash(std::size_t ncap) {
    std::vector<std::uint64_t> ok = std::move(keys_);
    std::vector<std::uint32_t> ov = std::move(vals_);
    cap_ = ncap; mask_ = ncap - 1; size_ = 0;
    keys_.assign(ncap, 0);
    vals_.assign(ncap, kEmpty);
    for (std::size_t i = 0; i < ov.size(); ++i)
      if (ov[i] != kEmpty) insert(ok[i], ov[i]);
  }

  std::vector<std::uint64_t> keys_;
  std::vector<std::uint32_t> vals_;
  std::size_t cap_{0}, mask_{0}, size_{0};
};

}  // namespace gavel
