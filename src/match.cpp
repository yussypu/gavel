#include <algorithm>
#include "gavel/engine.hpp"
#include "gavel/contracts.hpp"

namespace gavel {

void Engine::mark_touched_sym(SymbolIdx s) {
  Book& b = books_[s];
  if (!b.touched) {
    b.touched = true;
    touched_.push_back(s);
  }
}

namespace {
// Lazy cleanup: drop ids that no longer resolve to the expected order.
void compact_ids(std::vector<OrderId>& v, std::size_t dead) {
  if (dead * 2 < v.size()) return;
  v.erase(std::remove(v.begin(), v.end(), OrderId{0}), v.end());
}
bool price_allows(const Order& in, Price px) {
  if (in.kind == OrderKind::market) return true;
  return in.side == Side::buy ? in.price >= px : in.price <= px;
}
}  // namespace

// First live hidden midpoint order on side sd whose cap admits the midpoint.
std::uint32_t Engine::first_live_hidden(Book& b, Side sd, Price m_px, const Order& incoming) {
  auto& v = b.hidden_mids[static_cast<int>(sd)];
  std::size_t dead = 0;
  std::uint32_t found = kNil;
  for (auto& id : v) {
    if (id == 0) { ++dead; continue; }
    const std::uint32_t oi = ids_.find(id);
    if (oi == IdMap::kEmpty || ostate(pool_[oi]) != OrderState::resting) {
      id = 0;
      ++dead;
      continue;
    }
    const Order& o = pool_[oi];
    const bool cap_ok = !o.cap.valid() ||
                        (sd == Side::sell ? m_px >= o.cap : m_px <= o.cap);
    if (!cap_ok) continue;
    // FOK incoming coerces smp to cancel_resting; skip handled by caller.
    (void)incoming;
    found = oi;
    break;
  }
  compact_ids(v, dead);
  return found;
}

Qty Engine::fillable(const Order& in) const {
  // Displayed book only, by design; see semantics doc.
  const Book& b = books_[in.symbol];
  const LadderSide& opp = b.side(opposite(in.side));
  const bool exclude_self = in.smp != SmpPolicy::none;
  std::int64_t sum = 0;
  for (Price p = opp.best(); p.valid() && price_allows(in, p); p = opp.next_worse(p)) {
    const Level* l = opp.level_if(p);
    if (!l) continue;
    for (std::uint32_t i = l->head; i != kNil; i = pool_[i].next) {
      const Order& o = pool_[i];
      if (exclude_self && o.participant == in.participant) continue;
      sum += o.open();
      if (sum >= in.display_open) return in.display_open;
    }
  }
  return static_cast<Qty>(std::min<std::int64_t>(sum, in.display_open));
}

void Engine::execute(std::uint32_t ri, std::uint32_t ii, Price px, Qty q, std::uint8_t flags) {
  Order& r = pool_[ri];
  Order& in = pool_[ii];
  GAVEL_PRE(q > 0 && q <= r.display_open && q <= in.display_open);
  Book& b = books_[r.symbol];
  const bool hidden = flags & EvExecuted::kHidden;
  digest_remove(r);
  r.display_open -= q;
  r.cum_executed += q;
  if (!hidden && ostate(r) == OrderState::resting)
    b.side(r.side).on_qty_change(r, -q);
  in.display_open -= q;
  in.cum_executed += q;
  emitter_.emit(EventType::executed, EvExecuted{r.id, in.id, now_, px.v, q, r.open(), flags});
  b.last_trade = px;
  if (b.print_lo == 0 || px.v < b.print_lo) b.print_lo = px.v;
  if (px.v > b.print_hi) b.print_hi = px.v;
  mark_touched_sym(r.symbol);
  if (r.display_open == 0) {
    if (r.iceberg() && r.reserve > 0) {
      if (!hidden) b.side(r.side).unlink(pool_, ri);
      const Qty refill = std::min(r.display_size, r.reserve);
      r.display_open = refill;
      r.reserve -= refill;
      if (!hidden) b.side(r.side).push_back(pool_, ri);
      emitter_.emit(EventType::refilled, EvRefilled{r.id, now_, refill});
      digest_add(r);
    } else if (r.reserve == 0) {
      if (!hidden && ostate(r) == OrderState::resting) b.side(r.side).unlink(pool_, ri);
      ids_.erase(r.id);
      pool_.release(ri);
    } else {
      digest_add(r);
    }
  } else {
    digest_add(r);
  }
}

void Engine::match(std::uint32_t oi) {
  Order& in = pool_[oi];
  Book& b = books_[in.symbol];
  const Side opp = opposite(in.side);
  LadderSide& oside = b.side(opp);
  const bool buy = in.side == Side::buy;
  const bool fok = in.tif == Tif::fok;
  while (pool_[oi].display_open > 0) {
    const Price a = oside.best();
    const bool dis_ok = a.valid() && price_allows(pool_[oi], a);
    const Price m = b.mid();
    std::uint32_t hv = kNil;
    if (m.valid() && price_allows(pool_[oi], m)) hv = first_live_hidden(b, opp, m, pool_[oi]);
    const bool hid_ok = hv != kNil;
    if (!dis_ok && !hid_ok) break;
    const bool use_hidden = hid_ok && (!dis_ok || (buy ? m < a : m > a));
    const std::uint32_t vi = use_hidden ? hv : oside.front(a);
    Order& v = pool_[vi];
    const SmpPolicy pol = pool_[oi].smp;
    if (pol != SmpPolicy::none && v.participant == pool_[oi].participant) {
      // FOK incoming always cancels the resting side to keep its all or none promise.
      const SmpPolicy eff = fok ? SmpPolicy::cancel_resting : pol;
      if (eff == SmpPolicy::cancel_resting) {
        cancel_order(vi, Reason::smp);
        continue;
      }
      if (eff == SmpPolicy::cancel_both) cancel_order(vi, Reason::smp);
      Order& in2 = pool_[oi];
      emitter_.emit(EventType::canceled, EvCanceled{in2.id, now_, in2.display_open, Reason::smp});
      in2.display_open = 0;
      break;
    }
    const Qty q = std::min(pool_[oi].display_open, v.display_open);
    execute(vi, oi, use_hidden ? m : a, q,
            use_hidden ? EvExecuted::kHidden : std::uint8_t{0});
  }
}

bool Engine::rest(std::uint32_t oi) {
  Order& o = pool_[oi];
  Book& b = books_[o.symbol];
  LadderSide& ls = b.side(o.side);
  if (o.iceberg()) {
    const Qty total = o.display_open;
    o.display_open = std::min(o.display_size, total);
    o.reserve = total - o.display_open;
  }
  if (!ls.anchored()) ls.anchor(o.price.v);
  if (!ls.in_window(o.price)) {
    std::int32_t nb = (o.price.v - static_cast<std::int32_t>(cfg_.ladder_window / 2)) & ~1;
    if (nb < 2) nb = 2;
    if (!ls.reanchor(pool_, nb)) {
      emitter_.emit(EventType::canceled, EvCanceled{o.id, now_, o.open(), Reason::collar});
      ids_.erase(o.id);
      pool_.release(oi);
      return false;
    }
  }
  set_state(o, OrderState::resting);
  ls.push_back(pool_, oi);
  digest_add(o);
  mark_touched_sym(o.symbol);
  return true;
}

void Engine::run_stop_cascade() {
  for (;;) {
    triggered_.clear();
    for (std::size_t ti = 0; ti < touched_.size(); ++ti) {
      Book& b = books_[touched_[ti]];
      if (b.print_lo == 0 && b.print_hi == 0) continue;
      const std::int32_t hi = b.print_hi, lo = b.print_lo;
      b.print_hi = 0;
      b.print_lo = 0;
      for (const Side sd : {Side::buy, Side::sell}) {
        auto& v = b.stops[static_cast<int>(sd)];
        std::size_t dead = 0;
        for (auto& id : v) {
          if (id == 0) { ++dead; continue; }
          const std::uint32_t oi = ids_.find(id);
          if (oi == IdMap::kEmpty || ostate(pool_[oi]) != OrderState::pending_stop) {
            id = 0;
            ++dead;
            continue;
          }
          const Order& o = pool_[oi];
          const bool hit = sd == Side::buy ? hi >= o.trigger.v : (lo != 0 && lo <= o.trigger.v);
          if (hit) {
            triggered_.push_back(oi);
            id = 0;
            ++dead;
          }
        }
        compact_ids(v, dead);
      }
    }
    if (triggered_.empty()) return;
    std::sort(triggered_.begin(), triggered_.end(),
              [this](std::uint32_t x, std::uint32_t y) { return pool_[x].id < pool_[y].id; });
    for (const std::uint32_t oi : triggered_) {
      Order& o = pool_[oi];
      digest_remove(o);
      emitter_.emit(EventType::stop_triggered, EvStopTriggered{o.id, now_, o.trigger.v});
      if (o.kind == OrderKind::stop_limit) {
        o.kind = OrderKind::limit;
        o.price = o.cap;
      } else {
        o.kind = OrderKind::market;
        o.price = kNoPrice;
      }
      set_state(o, OrderState::dead);
      if (state_ == SessionState::open) {
        match(oi);
        Order& o2 = pool_[oi];
        if (o2.display_open > 0) {
          if (o2.kind == OrderKind::limit) {
            rest(oi);
            continue;
          }
          emitter_.emit(EventType::canceled,
                        EvCanceled{o2.id, now_, o2.display_open, Reason::no_liquidity});
          o2.display_open = 0;
        }
      } else {
        emitter_.emit(EventType::canceled, EvCanceled{o.id, now_, o.display_open, Reason::wrong_state});
        pool_[oi].display_open = 0;
      }
      if (pool_[oi].display_open == 0) {
        ids_.erase(pool_[oi].id);
        pool_.release(oi);
      }
    }
  }
}

Price Engine::peg_price(const Order& o, const Book& b) const {
  const bool buy = o.side == Side::buy;
  const Price ref = o.kind == OrderKind::peg_primary
                        ? b.side(o.side).best_nonpeg()
                        : b.side(opposite(o.side)).best_nonpeg();
  if (!ref.valid()) return kNoPrice;
  std::int32_t px = buy ? ref.v - o.peg_offset : ref.v + o.peg_offset;
  if (o.cap.valid()) px = buy ? std::min(px, o.cap.v) : std::max(px, o.cap.v);
  // Pegs are never marketable: clamp one tick passive of the opposite displayed best, including other pegs, so the displayed book never crosses.
  const Price ob = b.side(opposite(o.side)).best();
  if (ob.valid()) {
    if (buy && px >= ob.v) px = ob.v - 2;
    if (!buy && px <= ob.v) px = ob.v + 2;
  }
  if (px <= 0) return kNoPrice;
  return Price{px};
}

void Engine::reprice_symbol(SymbolIdx s) {
  Book& b = books_[s];
  auto& v = b.pegs;
  std::size_t dead = 0;
  for (auto& id : v) {
    if (id == 0) { ++dead; continue; }
    const std::uint32_t oi = ids_.find(id);
    if (oi == IdMap::kEmpty) { id = 0; ++dead; continue; }
    Order& o = pool_[oi];
    if (ostate(o) != OrderState::resting && ostate(o) != OrderState::parked) {
      id = 0;
      ++dead;
      continue;
    }
    const Price np = peg_price(o, b);
    const Price cur = ostate(o) == OrderState::parked ? kNoPrice : o.price;
    if (np == cur) continue;
    digest_remove(o);
    const std::int32_t old_px = cur.v;
    if (ostate(o) == OrderState::resting) b.side(o.side).unlink(pool_, oi);
    if (!np.valid()) {
      set_state(o, OrderState::parked);
      o.price = kNoPrice;
      digest_add(o);
      emitter_.emit(EventType::repriced, EvRepriced{o.id, now_, old_px, 0});
      continue;
    }
    o.price = np;
    set_state(o, OrderState::dead);  // rest() sets resting; keeps digest single entry
    if (rest(oi)) emitter_.emit(EventType::repriced, EvRepriced{o.id, now_, old_px, np.v});
  }
  compact_ids(v, dead);
}

void Engine::run_reprice_pass() {
  for (std::size_t i = 0; i < touched_.size(); ++i) reprice_symbol(touched_[i]);
}

void Engine::run_all_crosses(bool closing) {
  for (SymbolIdx s = 0; s < cfg_.num_symbols; ++s) run_cross(s, closing);
}

void Engine::run_cross(SymbolIdx s, bool closing) {
  (void)closing;
  Book& b = books_[s];
  // Gather eligible orders in allocation priority order: market FIFO, then price, then FIFO.
  cross_buys_.clear();
  cross_sells_.clear();
  std::int64_t mb = 0, ms = 0;
  for (const Side sd : {Side::buy, Side::sell}) {
    auto& q = b.market_queue[static_cast<int>(sd)];
    std::size_t dead = 0;
    for (auto& id : q) {
      if (id == 0) { ++dead; continue; }
      const std::uint32_t oi = ids_.find(id);
      if (oi == IdMap::kEmpty || ostate(pool_[oi]) != OrderState::queued_market) {
        id = 0;
        ++dead;
        continue;
      }
      (sd == Side::buy ? cross_buys_ : cross_sells_).push_back(oi);
      (sd == Side::buy ? mb : ms) += pool_[oi].open();
    }
    compact_ids(q, dead);
  }
  const std::size_t market_buys = cross_buys_.size(), market_sells = cross_sells_.size();
  b.bid.for_each_level([&](Price, const Level& l) {
    for (std::uint32_t i = l.head; i != kNil; i = pool_[i].next) cross_buys_.push_back(i);
  });
  b.ask.for_each_level([&](Price, const Level& l) {
    for (std::uint32_t i = l.head; i != kNil; i = pool_[i].next) cross_sells_.push_back(i);
  });
  if (cross_buys_.empty() && cross_sells_.empty()) return;

  // Candidate prices are displayed levels; demand falls and supply rises in price, so one sorted prefix sum sweep evaluates each in O(1).
  std::int64_t best_exec = 0, best_imb = 0;
  std::int32_t best_px = 0;
  const std::int32_t ref = b.last_trade.valid() ? b.last_trade.v : 0;
  // Aggregate displayed open quantity by price per side for an order independent sweep.
  cross_buy_px_.clear();
  cross_sell_px_.clear();
  for (std::size_t i = market_buys; i < cross_buys_.size(); ++i)
    cross_buy_px_.push_back({pool_[cross_buys_[i]].price.v, pool_[cross_buys_[i]].open()});
  for (std::size_t i = market_sells; i < cross_sells_.size(); ++i)
    cross_sell_px_.push_back({pool_[cross_sells_[i]].price.v, pool_[cross_sells_[i]].open()});
  auto by_px = [](const PxQty& a, const PxQty& c) { return a.px < c.px; };
  std::sort(cross_buy_px_.begin(), cross_buy_px_.end(), by_px);
  std::sort(cross_sell_px_.begin(), cross_sell_px_.end(), by_px);
  cross_px_.clear();
  for (const PxQty& e : cross_buy_px_) cross_px_.push_back(e.px);
  for (const PxQty& e : cross_sell_px_) cross_px_.push_back(e.px);
  std::sort(cross_px_.begin(), cross_px_.end());
  cross_px_.erase(std::unique(cross_px_.begin(), cross_px_.end()), cross_px_.end());

  auto consider = [&](std::int32_t px, std::int64_t demand, std::int64_t supply) {
    const std::int64_t ex = std::min(demand, supply);
    if (ex <= 0) return;
    const std::int64_t imb = demand > supply ? demand - supply : supply - demand;
    bool better = ex > best_exec;
    if (ex == best_exec) {
      if (imb < best_imb) better = true;
      else if (imb == best_imb && ref != 0 &&
               std::abs(px - ref) < std::abs(best_px - ref)) better = true;
      else if (imb == best_imb && (ref == 0 || std::abs(px - ref) == std::abs(best_px - ref)) &&
               px < best_px) better = true;
    }
    if (better) { best_exec = ex; best_imb = imb; best_px = px; }
  };
  // demand(px) = market buys plus displayed buys at price >= px; supply(px) = market sells plus displayed sells at price <= px.
  std::int64_t demand = mb, supply = ms;
  for (const PxQty& e : cross_buy_px_) demand += e.qty;
  std::size_t bi_sweep = 0, si_sweep = 0;
  for (std::int32_t px : cross_px_) {
    while (bi_sweep < cross_buy_px_.size() && cross_buy_px_[bi_sweep].px < px) {
      demand -= cross_buy_px_[bi_sweep].qty;
      ++bi_sweep;
    }
    while (si_sweep < cross_sell_px_.size() && cross_sell_px_[si_sweep].px <= px) {
      supply += cross_sell_px_[si_sweep].qty;
      ++si_sweep;
    }
    consider(px, demand, supply);
  }

  if (best_exec == 0) {
    emitter_.emit(EventType::auction_result, EvAuctionResult{now_, 0, mb - ms, 0, s});
    for (std::size_t i = 0; i < market_buys; ++i) cancel_order(cross_buys_[i], Reason::auction_unfilled);
    for (std::size_t i = 0; i < market_sells; ++i) cancel_order(cross_sells_[i], Reason::auction_unfilled);
    return;
  }

  const Price px{best_px};
  // Allocation: walk both sides in priority order, pairing fills at px.
  auto eligible = [&](std::size_t i, bool buy_side) {
    const auto& v = buy_side ? cross_buys_ : cross_sells_;
    const std::size_t mkt = buy_side ? market_buys : market_sells;
    const Order& o = pool_[v[i]];
    if (i < mkt) return true;
    return buy_side ? o.price.v >= best_px : o.price.v <= best_px;
  };
  auto fill_open = [&](std::uint32_t oi, Qty q) {
    // Auction fill consumes total open; re-split icebergs afterward.
    Order& o = pool_[oi];
    digest_remove(o);
    const bool in_ladder = ostate(o) == OrderState::resting;
    const Qty old_display = o.display_open;
    Qty take = std::min(q, o.display_open);
    o.display_open -= take;
    o.reserve -= q - take;
    o.cum_executed += q;
    const Qty total = o.open();
    if (total > 0 && o.iceberg()) {
      const Qty nd = std::min(o.display_size, total);
      o.reserve = total - nd;
      o.display_open = nd;
    }
    if (in_ladder) b.side(o.side).on_qty_change(o, o.display_open - old_display);
    if (total == 0) {
      if (in_ladder) b.side(o.side).unlink(pool_, oi);
      ids_.erase(o.id);
      pool_.release(oi);
    } else {
      digest_add(o);
    }
    return total;
  };
  // Both sides allocate exactly best_exec; planned per order fills applied on completion.
  std::int64_t rb = best_exec, rs = best_exec;
  std::size_t bi = 0, si = 0;
  std::uint32_t cur_b = kNil, cur_s = kNil;
  Qty bleft = 0, bplan = 0, sleft = 0, splan = 0;
  auto next_order = [&](bool buy_side, std::size_t& idx) -> std::uint32_t {
    const auto& v = buy_side ? cross_buys_ : cross_sells_;
    while (idx < v.size() && !eligible(idx, buy_side)) ++idx;
    return idx < v.size() ? v[idx++] : kNil;
  };
  for (;;) {
    if (bleft == 0) {
      if (cur_b != kNil) { fill_open(cur_b, bplan); cur_b = kNil; }
      if (rb > 0) {
        cur_b = next_order(true, bi);
        if (cur_b != kNil) bplan = bleft = static_cast<Qty>(std::min<std::int64_t>(pool_[cur_b].open(), rb));
      }
    }
    if (sleft == 0) {
      if (cur_s != kNil) { fill_open(cur_s, splan); cur_s = kNil; }
      if (rs > 0) {
        cur_s = next_order(false, si);
        if (cur_s != kNil) splan = sleft = static_cast<Qty>(std::min<std::int64_t>(pool_[cur_s].open(), rs));
      }
    }
    if (cur_b == kNil || cur_s == kNil) break;
    const Order& bo = pool_[cur_b];
    const Order& so = pool_[cur_s];
    const Qty q = std::min(bleft, sleft);
    emitter_.emit(EventType::executed,
                  EvExecuted{so.id, bo.id, now_, best_px, q,
                             static_cast<Qty>(so.open() - (splan - sleft + q)),
                             EvExecuted::kAuction});
    bleft -= q;
    sleft -= q;
    rb -= q;
    rs -= q;
  }
  if (cur_b != kNil) fill_open(cur_b, bplan - bleft);
  if (cur_s != kNil) fill_open(cur_s, splan - sleft);
  b.last_trade = px;
  if (b.print_lo == 0 || best_px < b.print_lo) b.print_lo = best_px;
  if (best_px > b.print_hi) b.print_hi = best_px;
  mark_touched_sym(s);
  emitter_.emit(EventType::auction_result,
                EvAuctionResult{now_, best_exec, /*imbalance*/ best_imb, best_px, s});
  // Unfilled market remainders cancel.
  for (std::size_t i = 0; i < market_buys; ++i) {
    const std::uint32_t oi = cross_buys_[i];
    if (pool_[oi].state != 0 && ostate(pool_[oi]) == OrderState::queued_market && pool_[oi].open() > 0)
      cancel_order(oi, Reason::auction_unfilled);
  }
  for (std::size_t i = 0; i < market_sells; ++i) {
    const std::uint32_t oi = cross_sells_[i];
    if (pool_[oi].state != 0 && ostate(pool_[oi]) == OrderState::queued_market && pool_[oi].open() > 0)
      cancel_order(oi, Reason::auction_unfilled);
  }
}

}  // namespace gavel
