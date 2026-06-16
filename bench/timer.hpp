#pragma once
#include <cstdint>

#if !defined(__aarch64__)
#error "bench/timer.hpp targets arm64 macOS (cntvct_el0)"
#endif

namespace gbench {

inline std::uint64_t now_ticks() {
  std::uint64_t t;
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
  return t;
}

inline std::uint64_t tick_freq() {
  std::uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

// On M1 cntfrq_el0 is 24 MHz, about 41.7 ns per tick; single shot readings below that are quantized.
inline double tick_resolution_ns() { return 1e9 / static_cast<double>(tick_freq()); }

inline std::uint64_t ticks_to_ns(std::uint64_t ticks) {
  static const std::uint64_t f = tick_freq();
  const unsigned __int128 n = static_cast<unsigned __int128>(ticks) * 1'000'000'000ull;
  return static_cast<std::uint64_t>(n / f);
}

// Times n iterations and returns mean ns per iteration; use for medians below timer resolution.
template <typename Fn>
double batched_ns_per_iter(Fn&& fn, std::uint64_t n) {
  const std::uint64_t t0 = now_ticks();
  for (std::uint64_t i = 0; i < n; ++i) fn();
  const std::uint64_t t1 = now_ticks();
  return static_cast<double>(ticks_to_ns(t1 - t0)) / static_cast<double>(n);
}

}  // namespace gbench
