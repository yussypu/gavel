#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gavel/input.hpp"
#include "gavel/stream.hpp"

namespace gbench {

// Deterministic seeded PRNG.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : x_(seed) {}
  std::uint64_t next() {
    x_ += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = x_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }

 private:
  std::uint64_t x_;
};

struct Workload {
  std::string name;
  std::vector<gavel::InputMsg> events;
};

namespace detail {

// Shared builder: tracks seq, ts, live order ids, and a random walking mid in ticks.
class Gen {
 public:
  Gen(std::uint64_t seed, std::size_t n) : rng_(seed) { out_.reserve(n + 64); }

  void session_open() {
    out_.push_back(gavel::make_clock(++seq_, ++ts_, gavel::ClockAction::session_start));
    out_.push_back(gavel::make_clock(++seq_, ++ts_, gavel::ClockAction::open_cross));
  }

  std::size_t size() const { return out_.size(); }
  std::vector<gavel::InputMsg> take() { return std::move(out_); }
  std::uint64_t pct() { return rng_.below(100); }

  void walk() {
    const std::uint64_t r = rng_.below(32);
    if (r == 0 && mid_ < 1400) ++mid_;
    else if (r == 1 && mid_ > 600) --mid_;
  }

  gavel::Qty lot() { return static_cast<gavel::Qty>(100 * (1 + rng_.below(10))); }

  void add_at(gavel::Side sd, std::int32_t ticks, gavel::Qty q) {
    out_.push_back(gavel::make_enter(++seq_, ++ts_, 0, sd, gavel::OrderKind::limit,
                                     gavel::Tif::day, q, ticks * 2));
    live_.push_back(seq_);
  }

  void add_near(std::int32_t max_off, gavel::Qty q) {
    const gavel::Side sd = rng_.below(2) ? gavel::Side::sell : gavel::Side::buy;
    const std::int32_t off = 1 + static_cast<std::int32_t>(rng_.below(static_cast<std::uint64_t>(max_off)));
    add_at(sd, sd == gavel::Side::buy ? mid_ - off : mid_ + off, q);
  }

  // IOC limit through the walked mid so it crosses whatever rests near the touch.
  void marketable(gavel::Qty q) {
    const gavel::Side sd = rng_.below(2) ? gavel::Side::sell : gavel::Side::buy;
    const std::int32_t ticks = sd == gavel::Side::buy ? mid_ + 4 : mid_ - 4;
    out_.push_back(gavel::make_enter(++seq_, ++ts_, 0, sd, gavel::OrderKind::limit,
                                     gavel::Tif::ioc, q, ticks * 2));
  }

  bool cancel() {
    if (live_.empty()) return false;
    const std::size_t i = static_cast<std::size_t>(rng_.below(live_.size()));
    out_.push_back(gavel::make_cancel(++seq_, ++ts_, live_[i]));
    live_[i] = live_.back();
    live_.pop_back();
    return true;
  }

  bool reduce() {
    if (live_.empty()) return false;
    const std::size_t i = static_cast<std::size_t>(rng_.below(live_.size()));
    out_.push_back(gavel::make_reduce(++seq_, ++ts_, live_[i], 100));
    return true;
  }

  bool replace() {
    if (live_.empty()) return false;
    const std::size_t i = static_cast<std::size_t>(rng_.below(live_.size()));
    const std::int32_t off = 1 + static_cast<std::int32_t>(rng_.below(8));
    const std::int32_t ticks = rng_.below(2) ? mid_ + off : mid_ - off;
    out_.push_back(gavel::make_replace(++seq_, ++ts_, live_[i], lot(), ticks * 2));
    live_[i] = seq_;  // the replacement order takes the new id
    return true;
  }

  void prelude_depth(std::int32_t levels, int per_level, gavel::Qty q) {
    for (std::int32_t d = 1; d <= levels; ++d)
      for (int k = 0; k < per_level; ++k) {
        add_at(gavel::Side::buy, mid_ - d, q);
        add_at(gavel::Side::sell, mid_ + d, q);
      }
  }

 private:
  SplitMix64 rng_;
  std::vector<gavel::InputMsg> out_;
  std::vector<gavel::OrderId> live_;
  gavel::Seq seq_{0};
  gavel::Ts ts_{0};
  std::int32_t mid_{1000};
};

}  // namespace detail

// Heavy add then cancel near the touch, about 10% marketable.
inline Workload make_add_cancel(std::uint64_t seed, std::size_t n) {
  detail::Gen g(seed, n);
  g.session_open();
  while (g.size() < n) {
    g.walk();
    const std::uint64_t r = g.pct();
    if (r < 10) g.marketable(g.lot());
    else if (r < 55) g.add_near(6, g.lot());
    else if (!g.cancel()) g.add_near(6, g.lot());
  }
  return {"add_cancel", g.take()};
}

// Deep book with bursts of large marketable sweeps.
inline Workload make_sweep_heavy(std::uint64_t seed, std::size_t n) {
  detail::Gen g(seed, n);
  g.session_open();
  if (n > 600) g.prelude_depth(40, 3, 5000);
  while (g.size() < n) {
    g.walk();
    const std::uint64_t r = g.pct();
    if (r < 4) {
      const std::uint64_t burst = 4 + g.pct() % 12;
      for (std::uint64_t b = 0; b < burst && g.size() < n; ++b)
        g.marketable(static_cast<gavel::Qty>(2000 + 1000 * (g.pct() % 8)));
    } else if (r < 14) {
      if (!g.cancel()) g.add_near(30, g.lot());
    } else {
      g.add_near(30, static_cast<gavel::Qty>(500 + 100 * (g.pct() % 20)));
    }
  }
  return {"sweep_heavy", g.take()};
}

// Approximate real venue ratios: 50% adds, 25% cancels, 10% reduces, 10% replaces, 5% marketable.
inline Workload make_mixed_realistic(std::uint64_t seed, std::size_t n) {
  detail::Gen g(seed, n);
  g.session_open();
  while (g.size() < n) {
    g.walk();
    const std::uint64_t r = g.pct();
    if (r < 50) g.add_near(10, g.lot());
    else if (r < 75) { if (!g.cancel()) g.add_near(10, g.lot()); }
    else if (r < 85) { if (!g.reduce()) g.add_near(10, g.lot()); }
    else if (r < 95) { if (!g.replace()) g.add_near(10, g.lot()); }
    else g.marketable(g.lot());
  }
  return {"mixed_realistic", g.take()};
}

// Loads a recorded .gvl input stream as a workload; returns false if unreadable.
inline bool load_stream(const std::string& path, Workload& out) {
  gavel::StreamReader in(path);
  if (!in.ok()) return false;
  out.name = "replay:" + path;
  out.events.clear();
  gavel::InputMsg m;
  while (in.read(m)) out.events.push_back(m);
  return true;
}

}  // namespace gbench
