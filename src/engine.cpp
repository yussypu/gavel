#include "gavel/engine.hpp"
#include <algorithm>
#include <cstring>
#include "gavel/contracts.hpp"

namespace gavel {

Engine::Engine(const Config& cfg)
    : cfg_(cfg), pool_(cfg.initial_order_capacity), books_(cfg.num_symbols) {
  for (auto& b : books_) b.init(cfg.ladder_window);
  emitter_.reserve(1 << 20);
  touched_.reserve(64);
  triggered_.reserve(64);
}

const Order* Engine::find_order(OrderId id) const {
  const std::uint32_t i = ids_.find(id);
  return i == IdMap::kEmpty ? nullptr : &pool_[i];
}

std::uint64_t Engine::order_digest(const Order& o) const {
  const std::uint64_t pw = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.price.v)) << 32) ^
                           static_cast<std::uint32_t>(o.open()) ^
                           (static_cast<std::uint64_t>(o.state) << 56);
  return mix64(o.id) ^ mix64(pw);
}

void Engine::on_msg(const InputMsg& m) {
  GAVEL_PRE(m.seq > last_seq_);
  last_seq_ = m.seq;
  now_ = m.ts;
  switch (m.type) {
    case MsgType::enter: do_enter(m); break;
    case MsgType::cancel: do_cancel(m); break;
    case MsgType::reduce: do_reduce(m); break;
    case MsgType::replace: do_replace(m); break;
    case MsgType::clock: do_clock(m); break;
    default: reject(m.seq, m.ts, Reason::bad_kind); break;
  }
  run_stop_cascade();
  run_reprice_pass();
  for (const SymbolIdx s : touched_) {
    Book& b = books_[s];
    b.touched = false;
    b.print_hi = 0;
    b.print_lo = 0;
  }
  touched_.clear();
  ++events_;
  if (cfg_.checkpoint_interval && events_ % cfg_.checkpoint_interval == 0) checkpoint(now_);
}

void Engine::checkpoint(Ts ts) {
  emitter_.emit(EventType::state_hash, EvStateHash{ts, events_, emitter_.hash(), digest_});
}

void Engine::reject(Seq seq, Ts ts, Reason r) {
  emitter_.emit(EventType::rejected, EvRejected{seq, ts, r});
}

void Engine::accept(const Order& o, Ts ts, bool display) {
  std::int32_t price = 0, aux = 0;
  switch (o.kind) {
    case OrderKind::limit: price = o.price.v; aux = o.display_size; break;
    case OrderKind::market: break;
    case OrderKind::peg_primary:
    case OrderKind::peg_market: price = o.cap.v; aux = o.peg_offset; break;
    case OrderKind::peg_mid: price = o.cap.v; break;
    case OrderKind::stop: aux = o.trigger.v; break;
    case OrderKind::stop_limit: price = o.cap.v; aux = o.trigger.v; break;
  }
  emitter_.emit(EventType::accepted,
                EvAccepted{o.id, ts, price, o.open(), aux, o.symbol, o.side, o.kind, o.tif,
                           static_cast<std::uint8_t>(display)});
}

bool Engine::validate_enter(const InputMsg& m, Reason& why) const {
  if (m.symbol >= cfg_.num_symbols) { why = Reason::bad_symbol; return false; }
  if (m.qty <= 0 || m.qty > 1'000'000'000) { why = Reason::bad_qty; return false; }
  if (static_cast<std::uint8_t>(m.kind) > static_cast<std::uint8_t>(OrderKind::stop_limit)) {
    why = Reason::bad_kind; return false;
  }
  if (static_cast<std::uint8_t>(m.tif) > static_cast<std::uint8_t>(Tif::fok)) {
    why = Reason::bad_tif; return false;
  }
  if (static_cast<std::uint8_t>(m.side) > 1) { why = Reason::bad_kind; return false; }
  switch (m.kind) {
    case OrderKind::limit:
      if (!m.limit_price().displayable()) { why = Reason::bad_price; return false; }
      if (m.aux < 0 || m.aux > m.qty) { why = Reason::bad_qty; return false; }
      break;
    case OrderKind::market:
      if (m.price != 0 || m.aux != 0) { why = Reason::bad_price; return false; }
      break;
    case OrderKind::peg_primary:
    case OrderKind::peg_market: {
      const std::int32_t min_off = m.kind == OrderKind::peg_market ? 2 : 0;
      if (m.aux < min_off || (m.aux & 1)) { why = Reason::bad_offset; return false; }
      if (m.price != 0 && !m.limit_price().displayable()) { why = Reason::bad_price; return false; }
      if (m.tif != Tif::day) { why = Reason::bad_tif; return false; }
      break;
    }
    case OrderKind::peg_mid:
      if (m.aux != 0) { why = Reason::bad_offset; return false; }
      if (m.price < 0) { why = Reason::bad_price; return false; }
      if (m.tif != Tif::day) { why = Reason::bad_tif; return false; }
      break;
    case OrderKind::stop:
      if (m.price != 0) { why = Reason::bad_price; return false; }
      if (!m.aux_price().displayable()) { why = Reason::bad_price; return false; }
      if (m.tif != Tif::day) { why = Reason::bad_tif; return false; }
      break;
    case OrderKind::stop_limit:
      if (!m.limit_price().displayable() || !m.aux_price().displayable()) {
        why = Reason::bad_price; return false;
      }
      if (m.tif != Tif::day) { why = Reason::bad_tif; return false; }
      break;
  }
  return true;
}

void Engine::do_enter(const InputMsg& m) {
  Reason why = Reason::none;
  if (state_ != SessionState::open && state_ != SessionState::pre_open) {
    reject(m.seq, m.ts, Reason::wrong_state);
    return;
  }
  if (!validate_enter(m, why)) {
    reject(m.seq, m.ts, why);
    return;
  }
  if (state_ == SessionState::pre_open) enter_preopen(m); else enter_continuous(m);
}

// Builds the order in the pool; caller routes it. Returns pool index.
static std::uint32_t build_order(OrderPool& pool, IdMap& ids, const InputMsg& m) {
  const std::uint32_t oi = pool.alloc();
  Order& o = pool[oi];
  o.id = m.seq;
  o.symbol = m.symbol;
  o.side = m.side;
  o.kind = m.kind;
  o.tif = m.tif;
  o.smp = m.smp();
  o.participant = m.participant;
  o.display_open = m.qty;
  o.reserve = 0;
  o.cum_executed = 0;
  switch (m.kind) {
    case OrderKind::limit:
      o.price = m.limit_price();
      o.display_size = m.aux;
      break;
    case OrderKind::market:
      o.price = kNoPrice;
      break;
    case OrderKind::peg_primary:
    case OrderKind::peg_market:
      o.cap = m.limit_price();
      o.peg_offset = m.aux;
      break;
    case OrderKind::peg_mid:
      o.cap = Price{m.price};
      break;
    case OrderKind::stop:
      o.trigger = m.aux_price();
      break;
    case OrderKind::stop_limit:
      o.cap = m.limit_price();
      o.trigger = m.aux_price();
      break;
  }
  ids.insert(o.id, oi);
  return oi;
}

void Engine::enter_preopen(const InputMsg& m) {
  if (m.kind == OrderKind::peg_primary || m.kind == OrderKind::peg_market ||
      m.kind == OrderKind::peg_mid) {
    reject(m.seq, m.ts, Reason::peg_in_auction);
    return;
  }
  if (m.tif != Tif::day) {
    reject(m.seq, m.ts, Reason::tif_in_auction);
    return;
  }
  const std::uint32_t oi = build_order(pool_, ids_, m);
  Order& o = pool_[oi];
  Book& b = books_[o.symbol];
  switch (m.kind) {
    case OrderKind::limit:
      accept(o, now_, true);
      rest(oi);
      break;
    case OrderKind::market:
      set_state(o, OrderState::queued_market);
      b.market_queue[static_cast<int>(o.side)].push_back(o.id);
      digest_add(o);
      accept(o, now_, false);
      break;
    case OrderKind::stop:
    case OrderKind::stop_limit:
      set_state(o, OrderState::pending_stop);
      b.stops[static_cast<int>(o.side)].push_back(o.id);
      digest_add(o);
      accept(o, now_, false);
      break;
    default:
      GAVEL_REQUIRE(false);
  }
}

void Engine::enter_continuous(const InputMsg& m) {
  const std::uint32_t oi = build_order(pool_, ids_, m);
  Order& o = pool_[oi];
  Book& b = books_[o.symbol];
  switch (m.kind) {
    case OrderKind::stop:
    case OrderKind::stop_limit:
      set_state(o, OrderState::pending_stop);
      b.stops[static_cast<int>(o.side)].push_back(o.id);
      digest_add(o);
      accept(o, now_, false);
      return;
    case OrderKind::peg_mid:
      set_state(o, OrderState::resting);
      b.hidden_mids[static_cast<int>(o.side)].push_back(o.id);
      digest_add(o);
      accept(o, now_, false);
      return;
    case OrderKind::peg_primary:
    case OrderKind::peg_market: {
      accept(o, now_, true);
      b.pegs.push_back(o.id);
      const Price px = peg_price(o, b);
      if (!px.valid()) {
        set_state(o, OrderState::parked);
        o.price = kNoPrice;
        digest_add(o);
        return;
      }
      o.price = px;
      if (rest(oi)) emitter_.emit(EventType::repriced, EvRepriced{o.id, now_, 0, px.v});
      return;
    }
    case OrderKind::market:
    case OrderKind::limit:
      break;
  }
  accept(o, now_, m.kind == OrderKind::limit);
  if (o.tif == Tif::fok) {
    if (fillable(o) < o.display_open) {
      emitter_.emit(EventType::canceled, EvCanceled{o.id, now_, o.display_open, Reason::fok_unfilled});
      ids_.erase(o.id);
      pool_.release(oi);
      return;
    }
  }
  match(oi);
  Order& o2 = pool_[oi];
  if (o2.display_open > 0) {
    if (o2.kind == OrderKind::limit && o2.tif == Tif::day) {
      rest(oi);
      return;
    }
    const Reason r = o2.kind == OrderKind::market ? Reason::no_liquidity : Reason::ioc_expired;
    emitter_.emit(EventType::canceled, EvCanceled{o2.id, now_, o2.display_open, r});
  }
  if (pool_[oi].display_open == 0 || pool_[oi].kind != OrderKind::limit ||
      pool_[oi].tif != Tif::day) {
    ids_.erase(pool_[oi].id);
    pool_.release(oi);
  }
}

void Engine::do_cancel(const InputMsg& m) {
  if (state_ != SessionState::open && state_ != SessionState::pre_open) {
    reject(m.seq, m.ts, Reason::wrong_state);
    return;
  }
  const std::uint32_t oi = ids_.find(m.target);
  if (oi == IdMap::kEmpty) {
    reject(m.seq, m.ts, Reason::unknown_target);
    return;
  }
  cancel_order(oi, Reason::user_cancel);
}

void Engine::do_reduce(const InputMsg& m) {
  if (state_ != SessionState::open && state_ != SessionState::pre_open) {
    reject(m.seq, m.ts, Reason::wrong_state);
    return;
  }
  if (m.qty <= 0) {
    reject(m.seq, m.ts, Reason::bad_qty);
    return;
  }
  const std::uint32_t oi = ids_.find(m.target);
  if (oi == IdMap::kEmpty) {
    reject(m.seq, m.ts, Reason::unknown_target);
    return;
  }
  Order& o = pool_[oi];
  if (m.qty >= o.open()) {
    cancel_order(oi, Reason::user_cancel);
    return;
  }
  digest_remove(o);
  Qty by = m.qty;
  const Qty from_reserve = std::min(o.reserve, by);
  o.reserve -= from_reserve;
  by -= from_reserve;
  if (by > 0) {
    o.display_open -= by;
    if (ostate(o) == OrderState::resting && o.kind != OrderKind::peg_mid) {
      books_[o.symbol].side(o.side).on_qty_change(o, -by);
      mark_touched_sym(o.symbol);
    }
  }
  digest_add(o);
  emitter_.emit(EventType::reduced, EvReduced{o.id, now_, m.qty, o.open()});
}

void Engine::do_replace(const InputMsg& m) {
  if (state_ != SessionState::open && state_ != SessionState::pre_open) {
    reject(m.seq, m.ts, Reason::wrong_state);
    return;
  }
  const std::uint32_t oi = ids_.find(m.target);
  if (oi == IdMap::kEmpty) {
    reject(m.seq, m.ts, Reason::unknown_target);
    return;
  }
  Order& old = pool_[oi];
  if (old.kind != OrderKind::limit || ostate(old) != OrderState::resting) {
    reject(m.seq, m.ts, Reason::bad_kind);
    return;
  }
  if (!Price{m.price}.displayable()) {
    reject(m.seq, m.ts, Reason::bad_price);
    return;
  }
  if (m.qty <= 0 || m.qty > 1'000'000'000) {
    reject(m.seq, m.ts, Reason::bad_qty);
    return;
  }
  InputMsg nm = m;
  nm.type = MsgType::enter;
  nm.kind = OrderKind::limit;
  nm.side = old.side;
  nm.symbol = old.symbol;
  nm.participant = old.participant;
  nm.tif = old.tif;
  nm.aux = old.display_size > 0 ? std::min<Qty>(old.display_size, m.qty) : 0;
  nm.flags = static_cast<std::uint8_t>(old.smp);
  emitter_.emit(EventType::replaced, EvReplaced{old.id, m.seq, now_});
  // Silent removal of the old order; the Replaced event covers it.
  digest_remove(old);
  remove_from_structure(oi);
  ids_.erase(old.id);
  pool_.release(oi);
  if (state_ == SessionState::pre_open) enter_preopen(nm); else enter_continuous(nm);
}

void Engine::do_clock(const InputMsg& m) {
  const auto action = static_cast<ClockAction>(m.clock_action);
  switch (action) {
    case ClockAction::session_start:
      if (state_ != SessionState::halted) { reject(m.seq, m.ts, Reason::wrong_state); return; }
      state_ = SessionState::pre_open;
      emitter_.emit(EventType::session_event, EvSessionEvent{now_, action});
      return;
    case ClockAction::open_cross:
      if (state_ != SessionState::pre_open) { reject(m.seq, m.ts, Reason::wrong_state); return; }
      emitter_.emit(EventType::session_event, EvSessionEvent{now_, action});
      state_ = SessionState::open;
      run_all_crosses(false);
      return;
    case ClockAction::close_cross: {
      if (state_ != SessionState::open) { reject(m.seq, m.ts, Reason::wrong_state); return; }
      run_all_crosses(true);
      // Day orders expire; cancel everything live in id order, then announce.
      std::vector<OrderId> live;
      for (std::uint32_t i = 0; i < pool_.size(); ++i)
        if (pool_[i].state != 0) live.push_back(pool_[i].id);
      std::sort(live.begin(), live.end());
      for (const OrderId id : live) {
        const std::uint32_t oi = ids_.find(id);
        if (oi != IdMap::kEmpty) cancel_order(oi, Reason::session_end);
      }
      state_ = SessionState::post_close;
      emitter_.emit(EventType::session_event, EvSessionEvent{now_, action});
      return;
    }
    case ClockAction::session_end:
      if (state_ != SessionState::post_close) { reject(m.seq, m.ts, Reason::wrong_state); return; }
      state_ = SessionState::ended;
      emitter_.emit(EventType::session_event, EvSessionEvent{now_, action});
      return;
  }
  reject(m.seq, m.ts, Reason::bad_kind);
}

void Engine::cancel_order(std::uint32_t oi, Reason r) {
  Order& o = pool_[oi];
  emitter_.emit(EventType::canceled, EvCanceled{o.id, now_, o.open(), r});
  digest_remove(o);
  remove_from_structure(oi);
  ids_.erase(o.id);
  pool_.release(oi);
}

void Engine::remove_from_structure(std::uint32_t oi) {
  Order& o = pool_[oi];
  if (ostate(o) == OrderState::resting && o.kind != OrderKind::peg_mid) {
    books_[o.symbol].side(o.side).unlink(pool_, oi);
    mark_touched_sym(o.symbol);
  }
  // Hidden, parked, pending and queued entries are id based and lazily skipped.
}

// Snapshot layout: header, ladder orders in FIFO walk order, remaining orders in id order.
namespace {
struct SnapHeader {
  std::uint64_t magic;
  std::uint32_t num_symbols;
  std::uint32_t ladder_window;
  SessionState state;
  std::uint8_t pad[7];
  Seq last_seq;
  Ts now;
  std::uint64_t events;
  std::uint64_t digest;
  std::uint64_t out_hash;
  std::uint64_t out_count;
  std::uint64_t order_count;
};
constexpr std::uint64_t kSnapMagic = 0x676176656c736e70ull;

template <typename T>
void put(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}
}  // namespace

void Engine::save_snapshot(std::vector<std::uint8_t>& out) const {
  out.clear();
  std::vector<Order> ladder_orders;
  std::vector<Order> other_orders;
  for (SymbolIdx s = 0; s < cfg_.num_symbols; ++s) {
    const Book& b = books_[s];
    for (const Side sd : {Side::buy, Side::sell}) {
      const LadderSide& ls = b.side(sd);
      ls.for_each_level([&](Price p, const Level& l) {
        (void)p;
        for (std::uint32_t i = l.head; i != kNil; i = pool_[i].next) ladder_orders.push_back(pool_[i]);
      });
    }
  }
  for (std::uint32_t i = 0; i < pool_.size(); ++i) {
    const Order& o = pool_[i];
    if (o.state == 0) continue;
    const bool in_ladder = ostate(o) == OrderState::resting && o.kind != OrderKind::peg_mid;
    if (!in_ladder) other_orders.push_back(o);
  }
  std::sort(other_orders.begin(), other_orders.end(),
            [](const Order& a, const Order& b) { return a.id < b.id; });
  SnapHeader h{};
  h.magic = kSnapMagic;
  h.num_symbols = cfg_.num_symbols;
  h.ladder_window = cfg_.ladder_window;
  h.state = state_;
  h.last_seq = last_seq_;
  h.now = now_;
  h.events = events_;
  h.digest = digest_;
  h.out_hash = emitter_.hash();
  h.out_count = emitter_.count();
  h.order_count = ladder_orders.size() + other_orders.size();
  put(out, h);
  for (SymbolIdx s = 0; s < cfg_.num_symbols; ++s) {
    const Book& b = books_[s];
    put(out, b.bid.base());
    put(out, b.ask.base());
    put(out, b.last_trade.v);
  }
  for (const Order& o : ladder_orders) put(out, o);
  for (const Order& o : other_orders) put(out, o);
}

void Engine::load_snapshot(const std::uint8_t* data, std::size_t n) {
  GAVEL_REQUIRE(n >= sizeof(SnapHeader));
  SnapHeader h;
  std::memcpy(&h, data, sizeof(h));
  GAVEL_REQUIRE(h.magic == kSnapMagic);
  GAVEL_REQUIRE(h.num_symbols == cfg_.num_symbols && h.ladder_window == cfg_.ladder_window);
  std::size_t off = sizeof(SnapHeader);
  state_ = h.state;
  last_seq_ = h.last_seq;
  now_ = h.now;
  events_ = h.events;
  for (SymbolIdx s = 0; s < cfg_.num_symbols; ++s) {
    std::int32_t bb, ab, lt;
    std::memcpy(&bb, data + off, 4); off += 4;
    std::memcpy(&ab, data + off, 4); off += 4;
    std::memcpy(&lt, data + off, 4); off += 4;
    books_[s].bid.set_base(bb);
    books_[s].ask.set_base(ab);
    books_[s].last_trade = Price{lt};
  }
  std::uint64_t check = 0;
  for (std::uint64_t k = 0; k < h.order_count; ++k) {
    GAVEL_REQUIRE(off + sizeof(Order) <= n);
    Order o;
    std::memcpy(&o, data + off, sizeof(Order));
    off += sizeof(Order);
    o.prev = o.next = kNil;
    const std::uint32_t oi = pool_.alloc();
    pool_[oi] = o;
    ids_.insert(o.id, oi);
    if (ostate(o) == OrderState::resting && o.kind != OrderKind::peg_mid)
      books_[o.symbol].side(o.side).push_back(pool_, oi);
    check ^= order_digest(o);
  }
  GAVEL_REQUIRE(check == h.digest);
  digest_ = h.digest;
  // Auxiliary lists are rebuilt in id order, which equals entry order.
  struct Entry { OrderId id; std::uint32_t oi; };
  std::vector<Entry> live;
  for (std::uint32_t i = 0; i < pool_.size(); ++i)
    if (pool_[i].state != 0) live.push_back({pool_[i].id, i});
  std::sort(live.begin(), live.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
  for (const Entry& e : live) {
    Order& o = pool_[e.oi];
    Book& b = books_[o.symbol];
    switch (ostate(o)) {
      case OrderState::resting:
        if (o.kind == OrderKind::peg_mid) b.hidden_mids[static_cast<int>(o.side)].push_back(o.id);
        else if (o.kind == OrderKind::peg_primary || o.kind == OrderKind::peg_market)
          b.pegs.push_back(o.id);
        break;
      case OrderState::parked: b.pegs.push_back(o.id); break;
      case OrderState::pending_stop: b.stops[static_cast<int>(o.side)].push_back(o.id); break;
      case OrderState::queued_market: b.market_queue[static_cast<int>(o.side)].push_back(o.id); break;
      default: GAVEL_REQUIRE(false);
    }
  }
  emitter_.restore(h.out_hash, h.out_count);
}

}  // namespace gavel
