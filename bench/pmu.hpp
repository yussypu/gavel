#pragma once
#include <dlfcn.h>
#include <cstdint>

namespace gbench {

// Best effort cycle and instruction counters via private kperf fixed counters (Lemire 2023); needs root, degrades to unavailable otherwise.

struct PmuCounts {
  std::uint64_t cycles{0};
  std::uint64_t instructions{0};
};

class Pmu {
 public:
  static Pmu& instance() {
    static Pmu p;
    return p;
  }

  bool available() const { return ok_; }

  void begin() {
    if (ok_) start_ = read();
  }

  // Deltas since begin(); zeros when unavailable.
  PmuCounts end() const {
    if (!ok_) return {};
    const PmuCounts now = read();
    return {now.cycles - start_.cycles, now.instructions - start_.instructions};
  }

  Pmu(const Pmu&) = delete;
  Pmu& operator=(const Pmu&) = delete;

 private:
  using SetCountingFn = int (*)(std::uint32_t);
  using GetCountersFn = int (*)(std::uint32_t, std::uint32_t, std::uint64_t*);
  using ForceSetFn = int (*)(int);
  using CounterCountFn = std::uint32_t (*)(std::uint32_t);

  static constexpr std::uint32_t kFixedMask = 1;  // KPC_CLASS_FIXED_MASK
  static constexpr std::uint32_t kMaxCounters = 32;

  Pmu() {
    void* h = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    if (!h) return;
    const auto force = reinterpret_cast<ForceSetFn>(dlsym(h, "kpc_force_all_ctrs_set"));
    const auto set_counting = reinterpret_cast<SetCountingFn>(dlsym(h, "kpc_set_counting"));
    const auto set_thread = reinterpret_cast<SetCountingFn>(dlsym(h, "kpc_set_thread_counting"));
    const auto counter_count = reinterpret_cast<CounterCountFn>(dlsym(h, "kpc_get_counter_count"));
    get_counters_ = reinterpret_cast<GetCountersFn>(dlsym(h, "kpc_get_thread_counters"));
    if (!force || !set_counting || !set_thread || !counter_count || !get_counters_) return;
    // Fails with a nonzero status when not running as root.
    if (force(1) != 0) return;
    if (set_counting(kFixedMask) != 0) return;
    if (set_thread(kFixedMask) != 0) return;
    n_ = counter_count(kFixedMask);
    if (n_ < 2 || n_ > kMaxCounters) return;
    std::uint64_t probe[kMaxCounters] = {};
    if (get_counters_(0, n_, probe) != 0) return;
    ok_ = true;
  }

  // Fixed counter 0 is core cycles, counter 1 is retired instructions on Apple Silicon.
  PmuCounts read() const {
    std::uint64_t buf[kMaxCounters] = {};
    if (get_counters_(0, n_, buf) != 0) return {};
    return {buf[0], buf[1]};
  }

  GetCountersFn get_counters_{nullptr};
  std::uint32_t n_{0};
  PmuCounts start_{};
  bool ok_{false};
};

}  // namespace gbench
