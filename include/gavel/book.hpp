#pragma once
#include <bit>
#include <cstdint>
#include <vector>
#include "gavel/types.hpp"
#include "gavel/pool.hpp"

namespace gavel {

// One displayed price level: FIFO of orders plus aggregates.
struct Level {
  std::int64_t agg_qty{0};
  std::uint32_t head{kNil};
  std::uint32_t tail{kNil};
  std::uint32_t count{0};
  std::uint32_t nonpeg_count{0};
};

// One side of a symbol ladder: contiguous levels over a price window.
class LadderSide {
 public:
  void init(Side side, std::uint32_t window) {
    side_ = side;
    window_ = window;
    levels_.assign(window, Level{});
    occ_.assign((window + 63) / 64, 0);
    nonpeg_occ_.assign((window + 63) / 64, 0);
  }

  bool anchored() const { return base_ != 0; }
  std::int32_t base() const { return base_; }
  // Snapshot restore only.
  void set_base(std::int32_t b) { base_ = b; }
  std::uint32_t window() const { return window_; }

  // Anchors so price sits mid window; price is in half ticks, base kept even.
  void anchor(std::int32_t price) {
    base_ = price - static_cast<std::int32_t>(window_ / 2);
    base_ &= ~1;
    if (base_ < 2) base_ = 2;
  }

  bool in_window(Price p) const {
    return base_ != 0 && p.v >= base_ && p.v < base_ + static_cast<std::int32_t>(window_);
  }

  Level& level(Price p) { return levels_[idx(p)]; }
  const Level* level_if(Price p) const {
    if (!in_window(p)) return nullptr;
    const Level& l = levels_[idx(p)];
    return l.count ? &l : nullptr;
  }

  void push_back(OrderPool& pool, std::uint32_t oi) {
    Order& o = pool[oi];
    Level& l = level(o.price);
    o.prev = l.tail; o.next = kNil;
    if (l.tail != kNil) pool[l.tail].next = oi; else l.head = oi;
    l.tail = oi;
    l.agg_qty += o.display_open;
    ++l.count;
    const bool nonpeg = o.kind != OrderKind::peg_primary && o.kind != OrderKind::peg_market;
    if (nonpeg && ++l.nonpeg_count == 1) set_bit(nonpeg_occ_, idx(o.price));
    if (l.count == 1) {
      set_bit(occ_, idx(o.price));
      update_best_on_add(idx(o.price));
    }
  }

  void unlink(OrderPool& pool, std::uint32_t oi) {
    Order& o = pool[oi];
    Level& l = level(o.price);
    if (o.prev != kNil) pool[o.prev].next = o.next; else l.head = o.next;
    if (o.next != kNil) pool[o.next].prev = o.prev; else l.tail = o.prev;
    l.agg_qty -= o.display_open;
    --l.count;
    const bool nonpeg = o.kind != OrderKind::peg_primary && o.kind != OrderKind::peg_market;
    if (nonpeg && --l.nonpeg_count == 0) clear_bit(nonpeg_occ_, idx(o.price));
    if (l.count == 0) {
      clear_bit(occ_, idx(o.price));
      if (static_cast<std::int32_t>(idx(o.price)) == best_) refresh_best();
    }
    o.prev = o.next = kNil;
  }

  void on_qty_change(const Order& o, Qty delta) { level(o.price).agg_qty += delta; }

  Price best() const { return best_ < 0 ? kNoPrice : Price{base_ + best_}; }

  Price best_nonpeg() const {
    const std::int32_t i = scan(nonpeg_occ_, best_);
    return i < 0 ? kNoPrice : Price{base_ + i};
  }

  // Next occupied level at a price strictly worse than p, walking away from the touch.
  Price next_worse(Price p) const {
    std::int32_t i = static_cast<std::int32_t>(idx(p));
    i = (side_ == Side::buy) ? prev_occupied(occ_, i - 1) : next_occupied(occ_, i + 1);
    return i < 0 ? kNoPrice : Price{base_ + i};
  }

  std::uint32_t front(Price p) { return level(p).head; }
  bool empty() const { return best_ < 0; }

  // Occupied prices from best to worst, for auction and introspection.
  template <typename Fn>
  void for_each_level(Fn&& fn) const {
    for (Price p = best(); p.valid(); p = next_worse(p)) fn(p, levels_[idx(p)]);
  }

  // True when every live order fits the window starting at new_base.
  bool reanchor(OrderPool& pool, std::int32_t new_base) {
    std::int32_t lo = 0, hi = -1;
    for (std::size_t w = 0; w < occ_.size(); ++w) {
      if (!occ_[w]) continue;
      const std::int32_t first = static_cast<std::int32_t>(w * 64) + std::countr_zero(occ_[w]);
      const std::int32_t last = static_cast<std::int32_t>(w * 64) + 63 - std::countl_zero(occ_[w]);
      if (hi < 0) lo = first;
      hi = last;
    }
    if (hi >= 0) {
      const std::int32_t lo_p = base_ + lo, hi_p = base_ + hi;
      if (lo_p < new_base || hi_p >= new_base + static_cast<std::int32_t>(window_)) return false;
    }
    std::vector<Level> nl(window_);
    std::vector<std::uint64_t> nocc((window_ + 63) / 64, 0), nnp((window_ + 63) / 64, 0);
    if (hi >= 0) {
      for (std::int32_t i = lo; i <= hi; ++i) {
        if (!test_bit(occ_, static_cast<std::uint32_t>(i))) continue;
        const auto ni = static_cast<std::size_t>(base_ + i - new_base);
        const auto si = static_cast<std::size_t>(i);
        nl[ni] = levels_[si];
        set_bit(nocc, static_cast<std::uint32_t>(ni));
        if (levels_[si].nonpeg_count) set_bit(nnp, static_cast<std::uint32_t>(ni));
      }
    }
    levels_.swap(nl); occ_.swap(nocc); nonpeg_occ_.swap(nnp);
    best_ = best_ < 0 ? -1 : best_ + base_ - new_base;
    base_ = new_base;
    (void)pool;
    return true;
  }

 private:
  std::uint32_t idx(Price p) const { return static_cast<std::uint32_t>(p.v - base_); }

  static void set_bit(std::vector<std::uint64_t>& b, std::uint32_t i) { b[i >> 6] |= 1ull << (i & 63); }
  static void clear_bit(std::vector<std::uint64_t>& b, std::uint32_t i) { b[i >> 6] &= ~(1ull << (i & 63)); }
  static bool test_bit(const std::vector<std::uint64_t>& b, std::uint32_t i) { return b[i >> 6] >> (i & 63) & 1; }

  static std::int32_t next_occupied(const std::vector<std::uint64_t>& b, std::int32_t from) {
    if (from < 0) from = 0;
    std::size_t w = static_cast<std::size_t>(from) >> 6;
    if (w >= b.size()) return -1;
    std::uint64_t cur = b[w] & (~0ull << (from & 63));
    for (;;) {
      if (cur) return static_cast<std::int32_t>(w * 64) + std::countr_zero(cur);
      if (++w >= b.size()) return -1;
      cur = b[w];
    }
  }

  static std::int32_t prev_occupied(const std::vector<std::uint64_t>& b, std::int32_t from) {
    if (from < 0) return -1;
    std::size_t w = static_cast<std::size_t>(from) >> 6;
    if (w >= b.size()) { w = b.size() - 1; from = static_cast<std::int32_t>(w * 64 + 63); }
    std::uint64_t cur = b[w] & (~0ull >> (63 - (from & 63)));
    for (;;) {
      if (cur) return static_cast<std::int32_t>(w * 64) + 63 - std::countl_zero(cur);
      if (w == 0) return -1;
      cur = b[--w];
    }
  }

  // Best occupied index in this side's priority direction, hinted by old best.
  std::int32_t scan(const std::vector<std::uint64_t>& b, std::int32_t hint) const {
    return side_ == Side::buy ? prev_occupied(b, hint < 0 ? static_cast<std::int32_t>(window_) - 1 : hint)
                              : next_occupied(b, hint < 0 ? 0 : hint);
  }

  void update_best_on_add(std::uint32_t i) {
    const std::int32_t si = static_cast<std::int32_t>(i);
    if (best_ < 0 || (side_ == Side::buy ? si > best_ : si < best_)) best_ = si;
  }

  void refresh_best() {
    best_ = (side_ == Side::buy) ? prev_occupied(occ_, best_) : next_occupied(occ_, best_);
  }

  Side side_{Side::buy};
  std::uint32_t window_{0};
  std::int32_t base_{0};
  std::int32_t best_{-1};
  std::vector<Level> levels_;
  std::vector<std::uint64_t> occ_;
  std::vector<std::uint64_t> nonpeg_occ_;
};

// Per symbol book: two ladder sides plus seq ordered aux lists holding order ids (not pool indices) so pool reuse cannot alias.
struct Book {
  LadderSide bid;
  LadderSide ask;
  // Lazily cleaned vectors in entry order; iteration skips dead ids.
  std::vector<OrderId> hidden_mids[2];
  std::vector<OrderId> pegs;
  std::vector<OrderId> stops[2];
  std::vector<OrderId> market_queue[2];
  Price last_trade;
  // Per event scratch: print range for stop triggers and a reprice flag.
  std::int32_t print_hi{0};
  std::int32_t print_lo{0};
  bool touched{false};

  void init(std::uint32_t window) {
    bid.init(Side::buy, window);
    ask.init(Side::sell, window);
  }
  LadderSide& side(Side s) { return s == Side::buy ? bid : ask; }
  const LadderSide& side(Side s) const { return s == Side::buy ? bid : ask; }

  // Midpoint in half ticks; integer because displayed prices are even.
  Price mid() const {
    const Price b = bid.best(), a = ask.best();
    if (!b.valid() || !a.valid()) return kNoPrice;
    return Price{(b.v + a.v) / 2};
  }
};

}  // namespace gavel
