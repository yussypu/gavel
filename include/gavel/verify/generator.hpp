#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "gavel/input.hpp"

namespace gavel::sim {

// splitmix64; bit identical across platforms, no std::mt19937.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed) {}
  std::uint64_t next() {
    s_ += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = s_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  std::uint32_t below(std::uint32_t n) {
    return n ? static_cast<std::uint32_t>(next() % n) : 0;
  }
  bool pct(std::uint32_t p) { return below(100) < p; }

 private:
  std::uint64_t s_;
};

struct GenConfig {
  std::uint32_t num_symbols{4};
  std::uint32_t num_participants{8};
  std::int32_t anchor{2000};       // price walk anchor in half ticks
  std::int32_t walk_range{120};    // max drift from anchor in half ticks
  std::int32_t spread_range{20};   // entry offset range around the walk in half ticks
  std::int32_t stop_band{12};      // stop trigger distance bound in half ticks
  std::uint32_t big_order_pct{4};
  std::uint32_t big_qty_mult{20};
  std::uint32_t marketable_pct{35};
  std::uint32_t invalid_pct{2};
  std::uint32_t iceberg_pct{10};
  std::uint32_t smp_pct{5};
  std::uint32_t w_enter{70}, w_cancel{14}, w_reduce{8}, w_replace{8};
  std::uint32_t w_limit{58}, w_market{10}, w_peg_primary{7}, w_peg_market{5}, w_peg_mid{6},
      w_stop{7}, w_stop_limit{7};
  std::uint32_t w_day{84}, w_ioc{12}, w_fok{4};
  std::uint32_t preopen_fraction_pct{0};  // share of flow before open_cross
};

inline bool known_preset(std::string_view name) {
  return name == "default" || name == "peg_heavy" || name == "stop_cascade" ||
         name == "iceberg_sweep" || name == "auction_mix" || name == "tight_book";
}

inline GenConfig preset_config(std::string_view name) {
  GenConfig c;
  if (name == "peg_heavy") {
    c.w_limit = 30; c.w_peg_primary = 22; c.w_peg_market = 16; c.w_peg_mid = 18;
    c.w_market = 8; c.w_stop = 3; c.w_stop_limit = 3;
  } else if (name == "stop_cascade") {
    c.w_limit = 40; c.w_stop = 20; c.w_stop_limit = 16; c.w_market = 14;
    c.w_peg_primary = 2; c.w_peg_market = 1; c.w_peg_mid = 1;
    c.stop_band = 4; c.big_order_pct = 10;
  } else if (name == "iceberg_sweep") {
    c.iceberg_pct = 60; c.w_limit = 62; c.w_market = 18; c.big_order_pct = 12;
    c.w_peg_primary = 3; c.w_peg_market = 2; c.w_peg_mid = 3; c.w_stop = 3; c.w_stop_limit = 3;
    c.marketable_pct = 45;
  } else if (name == "auction_mix") {
    c.preopen_fraction_pct = 25;
  } else if (name == "tight_book") {
    c.walk_range = 8; c.spread_range = 4; c.stop_band = 4; c.marketable_pct = 45;
  }
  return c;
}

// Seeded deterministic flow: seed in, InputMsg stream out, seq increasing, ts nondecreasing.
class Generator {
 public:
  Generator(std::uint64_t seed, const GenConfig& cfg)
      : rng_(seed), cfg_(cfg), walk_(cfg.num_symbols, cfg.anchor & ~1) {}

  std::vector<InputMsg> generate(std::uint64_t total) {
    if (total < 8) total = 8;
    std::vector<InputMsg> out;
    out.reserve(total);
    const std::uint64_t flow = total - 3;
    const std::uint64_t pre = flow * cfg_.preopen_fraction_pct / 100;
    push_clock(out, ClockAction::session_start);
    for (std::uint64_t i = 0; i < pre; ++i) push_flow(out, true);
    push_clock(out, ClockAction::open_cross);
    for (std::uint64_t i = 0; i < flow - pre; ++i) push_flow(out, false);
    push_clock(out, ClockAction::close_cross);
    return out;
  }

 private:
  struct LiveRef {
    OrderId id;
    SymbolIdx sym;
  };

  Seq next_seq() { return ++seq_; }
  void bump_ts() { ts_ += rng_.below(4); }

  void push_clock(std::vector<InputMsg>& out, ClockAction a) {
    bump_ts();
    out.push_back(make_clock(next_seq(), ts_, a));
  }

  std::int32_t walk_step(SymbolIdx s) {
    std::int32_t& w = walk_[s];
    w += 2 * (static_cast<std::int32_t>(rng_.below(5)) - 2);
    const std::int32_t lo = (cfg_.anchor - cfg_.walk_range) & ~1;
    const std::int32_t hi = (cfg_.anchor + cfg_.walk_range) & ~1;
    if (w < lo) w = lo;
    if (w > hi) w = hi;
    if (w < 4) w = 4;
    return w;
  }

  Qty rand_qty() {
    Qty q = 1 + static_cast<Qty>(rng_.below(200));
    if (rng_.pct(cfg_.big_order_pct)) q *= static_cast<Qty>(cfg_.big_qty_mult);
    return q;
  }

  std::int32_t cap_near(std::int32_t w) {
    const std::int32_t d = 2 * (static_cast<std::int32_t>(rng_.below(9)) - 4);
    const std::int32_t c = w + d;
    return c < 2 ? 2 : c;
  }

  std::int32_t trigger_near(std::int32_t w, bool buy) {
    const std::uint32_t band = static_cast<std::uint32_t>(cfg_.stop_band / 2);
    const std::int32_t d = 2 * (1 + static_cast<std::int32_t>(rng_.below(band ? band : 1)));
    const std::int32_t t = buy ? w + d : w - d;
    return t < 2 ? 2 : t;
  }

  OrderKind pick_kind() {
    const std::uint32_t tot = cfg_.w_limit + cfg_.w_market + cfg_.w_peg_primary +
                              cfg_.w_peg_market + cfg_.w_peg_mid + cfg_.w_stop + cfg_.w_stop_limit;
    std::uint32_t r = rng_.below(tot);
    if (r < cfg_.w_limit) return OrderKind::limit;
    r -= cfg_.w_limit;
    if (r < cfg_.w_market) return OrderKind::market;
    r -= cfg_.w_market;
    if (r < cfg_.w_peg_primary) return OrderKind::peg_primary;
    r -= cfg_.w_peg_primary;
    if (r < cfg_.w_peg_market) return OrderKind::peg_market;
    r -= cfg_.w_peg_market;
    if (r < cfg_.w_peg_mid) return OrderKind::peg_mid;
    r -= cfg_.w_peg_mid;
    if (r < cfg_.w_stop) return OrderKind::stop;
    return OrderKind::stop_limit;
  }

  Tif pick_tif() {
    std::uint32_t r = rng_.below(cfg_.w_day + cfg_.w_ioc + cfg_.w_fok);
    if (r < cfg_.w_day) return Tif::day;
    r -= cfg_.w_day;
    return r < cfg_.w_ioc ? Tif::ioc : Tif::fok;
  }

  void corrupt(InputMsg& m) {
    switch (rng_.below(4)) {
      case 0: m.price |= 1; break;
      case 1: m.qty = 0; break;
      case 2: m.symbol = static_cast<SymbolIdx>(cfg_.num_symbols); break;
      default: m.aux = -1; break;
    }
  }

  void remember(OrderId id, SymbolIdx sym) {
    if (live_.size() >= 65536) {
      const std::uint32_t j = rng_.below(static_cast<std::uint32_t>(live_.size()));
      live_[j] = live_.back();
      live_.pop_back();
    }
    live_.push_back({id, sym});
  }

  void push_enter(std::vector<InputMsg>& out, bool preopen) {
    const SymbolIdx sym = static_cast<SymbolIdx>(rng_.below(cfg_.num_symbols));
    const std::int32_t w = walk_step(sym);
    const Side side = rng_.below(2) ? Side::sell : Side::buy;
    const bool buy = side == Side::buy;
    OrderKind kind = pick_kind();
    if (preopen && (kind == OrderKind::peg_primary || kind == OrderKind::peg_market ||
                    kind == OrderKind::peg_mid))
      kind = OrderKind::limit;
    Tif tif = Tif::day;
    if (!preopen && (kind == OrderKind::limit || kind == OrderKind::market)) tif = pick_tif();
    const Qty qty = rand_qty();
    std::int32_t price = 0, aux = 0;
    switch (kind) {
      case OrderKind::limit: {
        const std::int32_t off =
            2 * static_cast<std::int32_t>(
                    rng_.below(static_cast<std::uint32_t>(cfg_.spread_range / 2) + 1));
        const bool agg = rng_.pct(cfg_.marketable_pct);
        price = buy ? (agg ? w + off : w - off) : (agg ? w - off : w + off);
        if (price < 2) price = 2;
        if (rng_.pct(cfg_.iceberg_pct))
          aux = 1 + static_cast<std::int32_t>(rng_.below(static_cast<std::uint32_t>(qty)));
        break;
      }
      case OrderKind::market:
        break;
      case OrderKind::peg_primary:
        aux = 2 * static_cast<std::int32_t>(rng_.below(4));
        if (rng_.pct(40)) price = cap_near(w);
        break;
      case OrderKind::peg_market:
        aux = 2 * (1 + static_cast<std::int32_t>(rng_.below(3)));
        if (rng_.pct(40)) price = cap_near(w);
        break;
      case OrderKind::peg_mid:
        if (rng_.pct(40)) price = cap_near(w);
        break;
      case OrderKind::stop:
        aux = trigger_near(w, buy);
        break;
      case OrderKind::stop_limit:
        aux = trigger_near(w, buy);
        price = buy ? aux + 2 * static_cast<std::int32_t>(rng_.below(4))
                    : aux - 2 * static_cast<std::int32_t>(rng_.below(4));
        if (price < 2) price = 2;
        break;
    }
    SmpPolicy smp = SmpPolicy::none;
    if (rng_.pct(cfg_.smp_pct)) smp = static_cast<SmpPolicy>(1 + rng_.below(3));
    InputMsg m = make_enter(next_seq(), ts_, sym, side, kind, tif, qty, price, aux,
                            static_cast<Participant>(rng_.below(cfg_.num_participants)), smp);
    if (rng_.pct(cfg_.invalid_pct)) corrupt(m);
    out.push_back(m);
    remember(m.seq, sym);
  }

  void push_flow(std::vector<InputMsg>& out, bool preopen) {
    bump_ts();
    const std::uint32_t tot = cfg_.w_enter + cfg_.w_cancel + cfg_.w_reduce + cfg_.w_replace;
    std::uint32_t r = rng_.below(tot);
    if (r < cfg_.w_enter || live_.empty()) {
      push_enter(out, preopen);
      return;
    }
    r -= cfg_.w_enter;
    const std::uint32_t pick = rng_.below(static_cast<std::uint32_t>(live_.size()));
    const LiveRef t = live_[pick];
    if (r < cfg_.w_cancel) {
      live_[pick] = live_.back();
      live_.pop_back();
      out.push_back(make_cancel(next_seq(), ts_, t.id));
      return;
    }
    r -= cfg_.w_cancel;
    if (r < cfg_.w_reduce) {
      out.push_back(make_reduce(next_seq(), ts_, t.id, 1 + static_cast<Qty>(rng_.below(60))));
      return;
    }
    live_[pick] = live_.back();
    live_.pop_back();
    const std::int32_t w = walk_step(t.sym);
    const std::int32_t half = cfg_.spread_range / 2;
    std::int32_t px =
        w + 2 * (static_cast<std::int32_t>(rng_.below(static_cast<std::uint32_t>(half) + 1)) -
                 half / 2);
    if (px < 2) px = 2;
    const Seq s = next_seq();
    out.push_back(make_replace(s, ts_, t.id, rand_qty(), px));
    remember(s, t.sym);
  }

  Rng rng_;
  GenConfig cfg_;
  std::vector<std::int32_t> walk_;
  std::vector<LiveRef> live_;
  Seq seq_{0};
  Ts ts_{0};
};

}  // namespace gavel::sim
