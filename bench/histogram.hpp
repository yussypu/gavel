#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gbench {

// Log linear latency histogram, 1 ns to 100 s, 64 sub buckets per octave, midpoint error under 1%.
class Histogram {
 public:
  static constexpr unsigned kSubBits = 7;
  static constexpr std::uint64_t kSub = 1ull << kSubBits;
  static constexpr std::uint64_t kMaxValue = 100'000'000'000ull;  // 100 s in ns
  static constexpr unsigned kMaxWidth = 37;  // bit width of kMaxValue
  static constexpr std::size_t kBuckets =
      static_cast<std::size_t>(kSub + (kMaxWidth - kSubBits) * (kSub / 2));

  void record(std::uint64_t ns) {
    if (ns > kMaxValue) ns = kMaxValue;  // out of range values clamp to 100 s
    ++counts_[index(ns)];
    ++count_;
    if (ns > max_) max_ = ns;
    if (ns < min_) min_ = ns;
  }

  std::uint64_t count() const { return count_; }
  std::uint64_t max() const { return count_ ? max_ : 0; }
  std::uint64_t min() const { return count_ ? min_ : 0; }

  // p in [0, 100]; returns a value within bucket precision of the exact percentile.
  std::uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    if (p >= 100.0) return max_;
    if (p < 0.0) p = 0.0;
    double rank = std::ceil(p / 100.0 * static_cast<double>(count_));
    if (rank < 1.0) rank = 1.0;
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      cum += counts_[i];
      if (static_cast<double>(cum) >= rank) return std::clamp(bucket_mid(i), min_, max_);
    }
    return max_;
  }

  void merge(const Histogram& o) {
    for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    count_ += o.count_;
    if (o.count_) {
      max_ = std::max(max_, o.max_);
      min_ = std::min(min_, o.min_);
    }
  }

 private:
  static std::size_t index(std::uint64_t v) {
    if (v < kSub) return static_cast<std::size_t>(v);
    const unsigned w = 64u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned j = w - kSubBits;
    const std::uint64_t sub = (v >> j) - kSub / 2;
    return static_cast<std::size_t>(kSub + (j - 1) * (kSub / 2) + sub);
  }

  static std::uint64_t bucket_mid(std::size_t i) {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const std::uint64_t r = static_cast<std::uint64_t>(i) - kSub;
    const unsigned j = static_cast<unsigned>(r / (kSub / 2)) + 1u;
    const std::uint64_t sub = kSub / 2 + r % (kSub / 2);
    return (sub << j) + (1ull << (j - 1));
  }

  // 2048 buckets at 4 bytes each, about 8 KiB.
  std::array<std::uint32_t, kBuckets> counts_{};
  std::uint64_t count_{0};
  std::uint64_t max_{0};
  std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
};

}  // namespace gbench
