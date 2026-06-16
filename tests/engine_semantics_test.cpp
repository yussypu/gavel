#include <cstring>
#include <set>
#include <vector>
#include "doctest.h"
#include "gavel/engine.hpp"

using namespace gavel;

namespace {

// Parsed output event: 1 byte type, 1 byte size, payload struct.
struct REv {
  EventType type{};
  std::vector<std::uint8_t> bytes;
  template <typename T>
  T as() const {
    REQUIRE(bytes.size() == sizeof(T));
    T v;
    std::memcpy(&v, bytes.data(), sizeof(T));
    return v;
  }
};

std::vector<REv> parse_events(const std::vector<std::uint8_t>& buf) {
  std::vector<REv> out;
  std::size_t i = 0;
  while (i < buf.size()) {
    REQUIRE(i + 2 <= buf.size());
    REv e;
    e.type = static_cast<EventType>(buf[i]);
    const std::size_t sz = buf[i + 1];
    REQUIRE(i + 2 + sz <= buf.size());
    e.bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(i + 2),
                   buf.begin() + static_cast<std::ptrdiff_t>(i + 2 + sz));
    out.push_back(std::move(e));
    i += 2 + sz;
  }
  return out;
}

std::vector<REv> filter(const std::vector<REv>& v, EventType t) {
  std::vector<REv> out;
  for (const auto& e : v)
    if (e.type == t) out.push_back(e);
  return out;
}

Config cfg(std::uint32_t symbols = 1, std::uint64_t checkpoint = 0) {
  return Config{symbols, 8192, 1 << 12, 1u << 26, checkpoint};
}

// Session helper; prices are passed in half ticks throughout.
struct Sess {
  Engine eng;
  Seq seq{0};
  Ts ts{0};
  explicit Sess(bool open = true, std::uint32_t symbols = 1, std::uint64_t checkpoint = 0)
      : eng(cfg(symbols, checkpoint)) {
    eng.on_msg(make_clock(++seq, ++ts, ClockAction::session_start));
    if (open) eng.on_msg(make_clock(++seq, ++ts, ClockAction::open_cross));
    eng.emitter().drain();
  }
  std::vector<REv> events() {
    auto v = parse_events(eng.emitter().buffer());
    eng.emitter().drain();
    return v;
  }
  Seq enter(Side sd, OrderKind k, Tif tif, Qty q, std::int32_t price, std::int32_t aux = 0,
            Participant part = 0, SmpPolicy smp = SmpPolicy::none, SymbolIdx sym = 0) {
    eng.on_msg(make_enter(++seq, ++ts, sym, sd, k, tif, q, price, aux, part, smp));
    return seq;
  }
  Seq limit(Side sd, Qty q, std::int32_t price, Tif tif = Tif::day, Participant part = 0,
            SmpPolicy smp = SmpPolicy::none) {
    return enter(sd, OrderKind::limit, tif, q, price, 0, part, smp);
  }
  Seq iceberg(Side sd, Qty q, std::int32_t price, Qty display) {
    return enter(sd, OrderKind::limit, Tif::day, q, price, display);
  }
  Seq market(Side sd, Qty q, Participant part = 0, SmpPolicy smp = SmpPolicy::none) {
    return enter(sd, OrderKind::market, Tif::day, q, 0, 0, part, smp);
  }
  Seq cancel(OrderId target) {
    eng.on_msg(make_cancel(++seq, ++ts, target));
    return seq;
  }
  Seq reduce(OrderId target, Qty by) {
    eng.on_msg(make_reduce(++seq, ++ts, target, by));
    return seq;
  }
  Seq replace(OrderId target, Qty q, std::int32_t price) {
    eng.on_msg(make_replace(++seq, ++ts, target, q, price));
    return seq;
  }
  Seq clock(ClockAction a) {
    eng.on_msg(make_clock(++seq, ++ts, a));
    return seq;
  }
  Reason lone_reject() {
    auto evs = events();
    REQUIRE(evs.size() == 1);
    REQUIRE(evs[0].type == EventType::rejected);
    return evs[0].as<EvRejected>().reason;
  }
  Qty level_qty(Side sd, std::int32_t price) const {
    Qty total = 0;
    eng.for_each_order_at(0, sd, Price{price}, [&](const Order& o) { total += o.display_open; });
    return total;
  }
};

}  // namespace

TEST_CASE("validation: every enter reject reason reachable") {
  Sess s;
  s.enter(Side::buy, OrderKind::limit, Tif::day, 10, 2000, 0, 0, SmpPolicy::none, 5);
  CHECK(s.lone_reject() == Reason::bad_symbol);
  s.limit(Side::buy, 10, 2001);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.limit(Side::buy, 10, 0);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.limit(Side::buy, 10, -2000);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.limit(Side::buy, 0, 2000);
  CHECK(s.lone_reject() == Reason::bad_qty);
  s.limit(Side::buy, -5, 2000);
  CHECK(s.lone_reject() == Reason::bad_qty);
  s.limit(Side::buy, 1'000'000'001, 2000);
  CHECK(s.lone_reject() == Reason::bad_qty);
  s.enter(Side::buy, static_cast<OrderKind>(9), Tif::day, 10, 2000);
  CHECK(s.lone_reject() == Reason::bad_kind);
  s.enter(Side::buy, OrderKind::limit, static_cast<Tif>(7), 10, 2000);
  CHECK(s.lone_reject() == Reason::bad_tif);
  // Peg offsets: parity and per kind minimums.
  s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 1);
  CHECK(s.lone_reject() == Reason::bad_offset);
  s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, -2);
  CHECK(s.lone_reject() == Reason::bad_offset);
  s.enter(Side::buy, OrderKind::peg_market, Tif::day, 10, 0, 0);
  CHECK(s.lone_reject() == Reason::bad_offset);
  s.enter(Side::buy, OrderKind::peg_market, Tif::day, 10, 0, 3);
  CHECK(s.lone_reject() == Reason::bad_offset);
  s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 2001, 2);
  CHECK(s.lone_reject() == Reason::bad_price);
  // Stop: price must be zero, trigger displayable.
  s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 2000, 2000);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2001);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.enter(Side::buy, OrderKind::stop_limit, Tif::day, 10, 1999, 2000);
  CHECK(s.lone_reject() == Reason::bad_price);
  // Market: price and aux must both be zero.
  s.enter(Side::buy, OrderKind::market, Tif::day, 10, 2000);
  CHECK(s.lone_reject() == Reason::bad_price);
  s.enter(Side::buy, OrderKind::market, Tif::day, 10, 0, 4);
  CHECK(s.lone_reject() == Reason::bad_price);
  // Iceberg display larger than total quantity.
  s.iceberg(Side::buy, 10, 2000, 11);
  CHECK(s.lone_reject() == Reason::bad_qty);
  // No side effects: book still empty.
  CHECK(s.eng.book(0).bid.empty());
  CHECK(s.eng.book(0).ask.empty());
}

TEST_CASE("price time priority: FIFO at a level, better price first, sweep") {
  Sess s;
  const Seq b1 = s.limit(Side::buy, 100, 2000);
  const Seq b2 = s.limit(Side::buy, 50, 2000);
  const Seq b3 = s.limit(Side::buy, 75, 1998);
  s.events();
  // FIFO at 2000: b1 fills fully before b2 sees a share.
  const Seq s1 = s.limit(Side::sell, 120, 1998);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  auto e0 = ex[0].as<EvExecuted>();
  auto e1 = ex[1].as<EvExecuted>();
  CHECK(e0.resting == b1);
  CHECK(e0.incoming == s1);
  CHECK(e0.qty == 100);
  CHECK(e0.price == 2000);
  CHECK(e0.resting_remaining == 0);
  CHECK(e1.resting == b2);
  CHECK(e1.qty == 20);
  CHECK(e1.price == 2000);
  CHECK(e1.resting_remaining == 30);
  // Better price first then sweep to the next level; remainder rests.
  const Seq s2 = s.limit(Side::sell, 200, 1996);
  ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  e0 = ex[0].as<EvExecuted>();
  e1 = ex[1].as<EvExecuted>();
  CHECK(e0.resting == b2);
  CHECK(e0.price == 2000);
  CHECK(e0.qty == 30);
  CHECK(e1.resting == b3);
  CHECK(e1.price == 1998);
  CHECK(e1.qty == 75);
  CHECK(s.eng.book(0).bid.empty());
  CHECK(s.eng.book(0).ask.best() == Price{1996});
  const Order* rem = s.eng.find_order(s2);
  REQUIRE(rem != nullptr);
  CHECK(rem->display_open == 95);
}

TEST_CASE("ioc: remainder cancels with ioc_expired and never rests") {
  Sess s;
  s.limit(Side::sell, 50, 2000);
  s.events();
  const Seq io = s.limit(Side::buy, 80, 2000, Tif::ioc);
  auto evs = s.events();
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().qty == 50);
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  auto c = cx[0].as<EvCanceled>();
  CHECK(c.id == io);
  CHECK(c.qty_canceled == 30);
  CHECK(c.reason == Reason::ioc_expired);
  CHECK(s.eng.find_order(io) == nullptr);
  CHECK(s.eng.book(0).bid.empty());
  // Fully filled ioc emits no cancel.
  s.limit(Side::sell, 10, 2000);
  s.events();
  s.limit(Side::buy, 10, 2000, Tif::ioc);
  CHECK(filter(s.events(), EventType::canceled).empty());
}

TEST_CASE("fok: full fill when exactly fillable, cancel when one share short") {
  SUBCASE("exactly fillable executes fully") {
    Sess s;
    s.limit(Side::sell, 30, 2000);
    s.limit(Side::sell, 70, 2002);
    s.events();
    const Seq f = s.limit(Side::buy, 100, 2002, Tif::fok);
    auto evs = s.events();
    auto ex = filter(evs, EventType::executed);
    REQUIRE(ex.size() == 2);
    CHECK(ex[0].as<EvExecuted>().qty == 30);
    CHECK(ex[1].as<EvExecuted>().qty == 70);
    CHECK(filter(evs, EventType::canceled).empty());
    CHECK(s.eng.find_order(f) == nullptr);
  }
  SUBCASE("one share short cancels in full and leaves the book intact") {
    Sess s;
    const Seq a1 = s.limit(Side::sell, 30, 2000);
    const Seq a2 = s.limit(Side::sell, 69, 2002);
    s.events();
    const Seq f = s.limit(Side::buy, 100, 2002, Tif::fok);
    auto evs = s.events();
    CHECK(filter(evs, EventType::executed).empty());
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    auto c = cx[0].as<EvCanceled>();
    CHECK(c.id == f);
    CHECK(c.qty_canceled == 100);
    CHECK(c.reason == Reason::fok_unfilled);
    REQUIRE(s.eng.find_order(a1) != nullptr);
    CHECK(s.eng.find_order(a1)->display_open == 30);
    CHECK(s.eng.find_order(a2)->display_open == 69);
  }
}

TEST_CASE("fok: hidden midpoint liquidity does not count toward fillability") {
  Sess s;
  s.limit(Side::buy, 10, 1990);
  s.limit(Side::sell, 99, 2010);
  // Hidden mid sell of 100 would cover the shortfall if counted.
  s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 100, 0);
  s.events();
  const Seq f = s.limit(Side::buy, 100, 2010, Tif::fok);
  auto evs = s.events();
  CHECK(filter(evs, EventType::executed).empty());
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == f);
  CHECK(cx[0].as<EvCanceled>().reason == Reason::fok_unfilled);
}

TEST_CASE("fok: smp coerced to cancel_resting keeps the all or none promise") {
  Sess s;
  const Seq self = s.limit(Side::sell, 50, 2000, Tif::day, 7);
  const Seq other = s.limit(Side::sell, 100, 2000, Tif::day, 8);
  s.events();
  // Policy says cancel_incoming, fok coerces to cancel the resting self order.
  const Seq f = s.limit(Side::buy, 100, 2000, Tif::fok, 7, SmpPolicy::cancel_incoming);
  auto evs = s.events();
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == self);
  CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().resting == other);
  CHECK(ex[0].as<EvExecuted>().incoming == f);
  CHECK(ex[0].as<EvExecuted>().qty == 100);
  CHECK(s.eng.find_order(f) == nullptr);
}

TEST_CASE("market orders: sweep, no_liquidity remainder, never rest, empty book") {
  Sess s;
  s.limit(Side::sell, 10, 2000);
  s.limit(Side::sell, 10, 2002);
  s.events();
  const Seq m = s.market(Side::buy, 25);
  auto evs = s.events();
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().price == 2000);
  CHECK(ex[1].as<EvExecuted>().price == 2002);
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == m);
  CHECK(cx[0].as<EvCanceled>().qty_canceled == 5);
  CHECK(cx[0].as<EvCanceled>().reason == Reason::no_liquidity);
  CHECK(s.eng.find_order(m) == nullptr);
  CHECK(s.eng.book(0).bid.empty());
  CHECK(s.eng.book(0).ask.empty());
  // Empty book: accepted then canceled in full.
  const Seq m2 = s.market(Side::sell, 40);
  evs = s.events();
  REQUIRE(evs.size() == 2);
  CHECK(evs[0].type == EventType::accepted);
  CHECK(evs[1].type == EventType::canceled);
  auto c2 = evs[1].as<EvCanceled>();
  CHECK(c2.id == m2);
  CHECK(c2.qty_canceled == 40);
  CHECK(c2.reason == Reason::no_liquidity);
}

TEST_CASE("icebergs: rest split, tail refill, Refilled event, reduce from reserve") {
  Sess s;
  const Seq ice = s.iceberg(Side::sell, 100, 2000, 30);
  auto acc = filter(s.events(), EventType::accepted);
  REQUIRE(acc.size() == 1);
  CHECK(acc[0].as<EvAccepted>().qty == 100);
  CHECK(acc[0].as<EvAccepted>().aux == 30);
  const Order* o = s.eng.find_order(ice);
  REQUIRE(o != nullptr);
  CHECK(o->display_open == 30);
  CHECK(o->reserve == 70);
  CHECK(s.level_qty(Side::sell, 2000) == 30);
  const Seq later = s.limit(Side::sell, 50, 2000);
  s.events();
  // Display executes fully, refills, and the refill goes to the level tail.
  s.limit(Side::buy, 30, 2000);
  auto evs = s.events();
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().resting == ice);
  CHECK(ex[0].as<EvExecuted>().resting_remaining == 70);
  auto rf = filter(evs, EventType::refilled);
  REQUIRE(rf.size() == 1);
  CHECK(rf[0].as<EvRefilled>().id == ice);
  CHECK(rf[0].as<EvRefilled>().display_qty == 30);
  o = s.eng.find_order(ice);
  REQUIRE(o != nullptr);
  CHECK(o->display_open == 30);
  CHECK(o->reserve == 40);
  // Queue position check: the later order now executes before the iceberg.
  s.limit(Side::buy, 40, 2000);
  ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().resting == later);
  CHECK(ex[0].as<EvExecuted>().qty == 40);
  // Sweep across the refill boundary within one event.
  s.limit(Side::buy, 50, 2000);
  evs = s.events();
  ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 3);
  CHECK(ex[0].as<EvExecuted>().resting == later);
  CHECK(ex[0].as<EvExecuted>().qty == 10);
  CHECK(ex[1].as<EvExecuted>().resting == ice);
  CHECK(ex[1].as<EvExecuted>().qty == 30);
  CHECK(ex[2].as<EvExecuted>().resting == ice);
  CHECK(ex[2].as<EvExecuted>().qty == 10);
  CHECK(filter(evs, EventType::refilled).size() == 1);
  o = s.eng.find_order(ice);
  REQUIRE(o != nullptr);
  CHECK(o->display_open == 20);
  CHECK(o->reserve == 10);
  // Reduce takes reserve first then display.
  s.reduce(ice, 15);
  auto rd = filter(s.events(), EventType::reduced);
  REQUIRE(rd.size() == 1);
  CHECK(rd[0].as<EvReduced>().qty_reduced == 15);
  CHECK(rd[0].as<EvReduced>().remaining == 15);
  o = s.eng.find_order(ice);
  REQUIRE(o != nullptr);
  CHECK(o->reserve == 0);
  CHECK(o->display_open == 15);
}

TEST_CASE("icebergs: replace carries display size") {
  Sess s;
  const Seq ice = s.iceberg(Side::sell, 100, 2000, 30);
  s.events();
  const Seq rid = s.replace(ice, 80, 2002);
  auto evs = s.events();
  auto rp = filter(evs, EventType::replaced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvReplaced>().old_id == ice);
  CHECK(rp[0].as<EvReplaced>().new_id == rid);
  auto acc = filter(evs, EventType::accepted);
  REQUIRE(acc.size() == 1);
  CHECK(acc[0].as<EvAccepted>().id == rid);
  CHECK(acc[0].as<EvAccepted>().qty == 80);
  CHECK(acc[0].as<EvAccepted>().aux == 30);
  CHECK(s.eng.find_order(ice) == nullptr);
  const Order* o = s.eng.find_order(rid);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2002});
  CHECK(o->display_open == 30);
  CHECK(o->reserve == 50);
}

TEST_CASE("pegs: primary references same side non peg best and parks without one") {
  Sess s;
  // A lone peg has no non peg reference and must park rather than self reference.
  const Seq peg = s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 2);
  auto evs = s.events();
  REQUIRE(filter(evs, EventType::accepted).size() == 1);
  CHECK(s.eng.book(0).bid.empty());
  const Order* o = s.eng.find_order(peg);
  REQUIRE(o != nullptr);
  CHECK(ostate(*o) == OrderState::parked);
  CHECK(o->price == kNoPrice);
  // A non peg bid appears: the peg unparks at ref minus offset.
  const Seq lim = s.limit(Side::buy, 10, 2000);
  evs = s.events();
  auto rp = filter(evs, EventType::repriced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvRepriced>().id == peg);
  CHECK(rp[0].as<EvRepriced>().old_price == 0);
  CHECK(rp[0].as<EvRepriced>().new_price == 1998);
  CHECK(s.level_qty(Side::buy, 1998) == 10);
  // The only non peg order cancels: the peg parks again with price 0.
  s.cancel(lim);
  evs = s.events();
  rp = filter(evs, EventType::repriced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvRepriced>().id == peg);
  CHECK(rp[0].as<EvRepriced>().old_price == 1998);
  CHECK(rp[0].as<EvRepriced>().new_price == 0);
  CHECK(s.eng.book(0).bid.empty());
  o = s.eng.find_order(peg);
  REQUIRE(o != nullptr);
  CHECK(ostate(*o) == OrderState::parked);
}

TEST_CASE("pegs: market peg references opposite side, offset and cap applied") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  s.limit(Side::sell, 10, 2010);
  s.events();
  const Seq pm = s.enter(Side::buy, OrderKind::peg_market, Tif::day, 5, 0, 2);
  s.events();
  const Order* o = s.eng.find_order(pm);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2008});
  CHECK(s.level_qty(Side::buy, 2008) == 5);
  // Cap binds below the pegged price.
  const Seq pc = s.enter(Side::buy, OrderKind::peg_market, Tif::day, 5, 2004, 2);
  s.events();
  o = s.eng.find_order(pc);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2004});
  // Sell mirrors get a fresh book so the buy pegs above do not move the touch.
  Sess s2;
  s2.limit(Side::buy, 10, 2000);
  s2.limit(Side::sell, 10, 2010);
  s2.events();
  // Sell market peg references the non peg bid plus offset.
  const Seq ps = s2.enter(Side::sell, OrderKind::peg_market, Tif::day, 5, 0, 4);
  s2.events();
  o = s2.eng.find_order(ps);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2004});
  // Primary sell references the non peg ask plus offset.
  const Seq pp = s2.enter(Side::sell, OrderKind::peg_primary, Tif::day, 5, 0, 2);
  s2.events();
  o = s2.eng.find_order(pp);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2012});
}

TEST_CASE("pegs: clamp one tick passive of the opposite touch") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  s.limit(Side::sell, 10, 2010);
  // Sell market peg rests at bid plus 2, inside the spread.
  s.enter(Side::sell, OrderKind::peg_market, Tif::day, 5, 0, 2);
  s.events();
  CHECK(s.eng.book(0).ask.best() == Price{2002});
  // Buy market peg computes 2008 off the non peg ask and clamps to 2000.
  const Seq pb = s.enter(Side::buy, OrderKind::peg_market, Tif::day, 5, 0, 2);
  auto rp = filter(s.events(), EventType::repriced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvRepriced>().id == pb);
  CHECK(rp[0].as<EvRepriced>().new_price == 2000);
  const Order* o = s.eng.find_order(pb);
  REQUIRE(o != nullptr);
  CHECK(o->price == Price{2000});
  // Nothing executed: pegs are never marketable on entry.
  CHECK(s.eng.book(0).last_trade == kNoPrice);
}

TEST_CASE("pegs: reprice moves the peg to the tail of its new level") {
  Sess s;
  const Seq a = s.limit(Side::buy, 10, 2000);
  const Seq peg = s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 20, 0, 4);
  const Seq c = s.limit(Side::buy, 30, 1996);
  s.events();
  CHECK(s.eng.find_order(peg)->price == Price{1996});
  // Move the reference away and back; the peg returns to 1996 behind c.
  s.cancel(a);
  auto rp = filter(s.events(), EventType::repriced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvRepriced>().new_price == 1992);
  const Seq d = s.limit(Side::buy, 10, 2000);
  rp = filter(s.events(), EventType::repriced);
  REQUIRE(rp.size() == 1);
  CHECK(rp[0].as<EvRepriced>().new_price == 1996);
  // Sweep: d first by price, then c before the peg at 1996.
  s.limit(Side::sell, 45, 1996);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 3);
  CHECK(ex[0].as<EvExecuted>().resting == d);
  CHECK(ex[1].as<EvExecuted>().resting == c);
  CHECK(ex[2].as<EvExecuted>().resting == peg);
  CHECK(ex[2].as<EvExecuted>().qty == 5);
}

TEST_CASE("pegs: rejected in pre open") {
  Sess s(false);
  s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 2);
  CHECK(s.lone_reject() == Reason::peg_in_auction);
  s.enter(Side::buy, OrderKind::peg_market, Tif::day, 10, 0, 2);
  CHECK(s.lone_reject() == Reason::peg_in_auction);
  s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0);
  CHECK(s.lone_reject() == Reason::peg_in_auction);
}

TEST_CASE("midpoint pegs: hidden, odd half tick execution, kHidden flag") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  const Seq ask = s.limit(Side::sell, 10, 2002);
  s.events();
  const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 0);
  auto evs = s.events();
  REQUIRE(filter(evs, EventType::accepted).size() == 1);
  CHECK(filter(evs, EventType::accepted)[0].as<EvAccepted>().display == 0);
  // No book delta: displayed ask unchanged and the level holds only the limit.
  CHECK(s.eng.book(0).ask.best() == Price{2002});
  CHECK(s.level_qty(Side::sell, 2002) == 10);
  // Incoming buy at 2002 prefers the midpoint 2001 over the worse displayed price.
  const Seq b = s.limit(Side::buy, 30, 2002);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  auto h = ex[0].as<EvExecuted>();
  CHECK(h.resting == hid);
  CHECK(h.incoming == b);
  CHECK(h.price == 2001);
  CHECK((h.price & 1) == 1);
  CHECK(h.qty == 20);
  CHECK((h.flags & EvExecuted::kHidden) != 0);
  auto d = ex[1].as<EvExecuted>();
  CHECK(d.resting == ask);
  CHECK(d.price == 2002);
  CHECK(d.qty == 10);
  CHECK((d.flags & EvExecuted::kHidden) == 0);
}

TEST_CASE("midpoint pegs: cap excludes execution and both sides are required") {
  SUBCASE("cap excluding the midpoint blocks the hidden order") {
    Sess s;
    s.limit(Side::buy, 10, 2000);
    const Seq ask = s.limit(Side::sell, 10, 2002);
    // Sell mid with floor 2003 sits above the 2001 midpoint.
    const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 2003);
    s.events();
    s.limit(Side::buy, 10, 2002);
    auto ex = filter(s.events(), EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == ask);
    CHECK(ex[0].as<EvExecuted>().price == 2002);
    CHECK(s.eng.find_order(hid) != nullptr);
  }
  SUBCASE("cap exactly at the midpoint allows execution") {
    Sess s;
    s.limit(Side::buy, 10, 2000);
    s.limit(Side::sell, 10, 2002);
    const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 5, 2001);
    s.events();
    s.limit(Side::buy, 5, 2002);
    auto ex = filter(s.events(), EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == hid);
    CHECK(ex[0].as<EvExecuted>().price == 2001);
  }
  SUBCASE("missing displayed bid parks midpoint execution") {
    Sess s;
    const Seq ask = s.limit(Side::sell, 10, 2002);
    const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 0);
    s.events();
    // Only the displayed ask trades; the hidden order is unmatchable.
    const Seq m = s.market(Side::buy, 15);
    auto evs = s.events();
    auto ex = filter(evs, EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == ask);
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == m);
    CHECK(cx[0].as<EvCanceled>().qty_canceled == 5);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::no_liquidity);
    CHECK(s.eng.find_order(hid) != nullptr);
  }
}

TEST_CASE("midpoint pegs: FIFO among hidden orders") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  s.limit(Side::sell, 100, 2002);
  const Seq h1 = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0);
  const Seq h2 = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0);
  s.events();
  s.limit(Side::buy, 15, 2002);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().resting == h1);
  CHECK(ex[0].as<EvExecuted>().qty == 10);
  CHECK(ex[1].as<EvExecuted>().resting == h2);
  CHECK(ex[1].as<EvExecuted>().qty == 5);
  CHECK((ex[1].as<EvExecuted>().flags & EvExecuted::kHidden) != 0);
}

TEST_CASE("stops: trigger on prints, not on entry against last_trade") {
  Sess s;
  s.limit(Side::sell, 10, 2000);
  const Seq far = s.limit(Side::sell, 100, 2004);
  s.limit(Side::buy, 10, 2000);
  s.events();
  CHECK(s.eng.book(0).last_trade == Price{2000});
  // Entry with last_trade already at the trigger must not fire.
  const Seq st = s.enter(Side::buy, OrderKind::stop, Tif::day, 20, 0, 2000);
  auto evs = s.events();
  CHECK(filter(evs, EventType::stop_triggered).empty());
  const Order* o = s.eng.find_order(st);
  REQUIRE(o != nullptr);
  CHECK(ostate(*o) == OrderState::pending_stop);
  // A fresh print at the trigger fires it; it becomes a market order.
  s.limit(Side::sell, 5, 2000);
  s.events();
  s.limit(Side::buy, 5, 2000);
  evs = s.events();
  auto tg = filter(evs, EventType::stop_triggered);
  REQUIRE(tg.size() == 1);
  CHECK(tg[0].as<EvStopTriggered>().id == st);
  CHECK(tg[0].as<EvStopTriggered>().trigger_price == 2000);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[1].as<EvExecuted>().resting == far);
  CHECK(ex[1].as<EvExecuted>().incoming == st);
  CHECK(ex[1].as<EvExecuted>().qty == 20);
  CHECK(ex[1].as<EvExecuted>().price == 2004);
}

TEST_CASE("stops: sell stop triggers at or below, not above") {
  Sess s;
  const Seq bid = s.limit(Side::buy, 100, 2000);
  const Seq st = s.enter(Side::sell, OrderKind::stop, Tif::day, 10, 0, 2004);
  s.events();
  // Print above the trigger: no fire.
  s.limit(Side::sell, 5, 2006);
  s.limit(Side::buy, 5, 2006);
  CHECK(filter(s.events(), EventType::stop_triggered).empty());
  // Print exactly at the trigger fires; the stop sells into the bid.
  s.limit(Side::sell, 5, 2004);
  s.events();
  s.limit(Side::buy, 5, 2004);
  auto evs = s.events();
  auto tg = filter(evs, EventType::stop_triggered);
  REQUIRE(tg.size() == 1);
  CHECK(tg[0].as<EvStopTriggered>().id == st);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[1].as<EvExecuted>().resting == bid);
  CHECK(ex[1].as<EvExecuted>().incoming == st);
  CHECK(ex[1].as<EvExecuted>().price == 2000);
}

TEST_CASE("stops: cascade orders conversions by id") {
  Sess s;
  s.limit(Side::sell, 50, 2008);
  const Seq s1 = s.enter(Side::buy, OrderKind::stop, Tif::day, 5, 0, 2004);
  const Seq s2 = s.enter(Side::buy, OrderKind::stop, Tif::day, 5, 0, 2004);
  s.limit(Side::sell, 5, 2004);
  s.events();
  s.limit(Side::buy, 5, 2004);
  auto evs = s.events();
  // Expect: trigger print exec, then s1 trigger and fill, then s2 trigger and fill.
  std::vector<OrderId> trig_ids;
  for (const auto& e : evs)
    if (e.type == EventType::stop_triggered) trig_ids.push_back(e.as<EvStopTriggered>().id);
  REQUIRE(trig_ids.size() == 2);
  CHECK(trig_ids[0] == s1);
  CHECK(trig_ids[1] == s2);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 3);
  CHECK(ex[1].as<EvExecuted>().incoming == s1);
  CHECK(ex[2].as<EvExecuted>().incoming == s2);
}

TEST_CASE("stops: cascade executions trigger further stops") {
  Sess s;
  s.limit(Side::sell, 5, 2004);
  s.limit(Side::sell, 5, 2008);
  s.limit(Side::sell, 5, 2012);
  const Seq s1 = s.enter(Side::buy, OrderKind::stop, Tif::day, 5, 0, 2004);
  const Seq s2 = s.enter(Side::buy, OrderKind::stop, Tif::day, 5, 0, 2008);
  s.events();
  s.limit(Side::buy, 5, 2004);
  auto evs = s.events();
  std::vector<OrderId> trig_ids;
  for (const auto& e : evs)
    if (e.type == EventType::stop_triggered) trig_ids.push_back(e.as<EvStopTriggered>().id);
  REQUIRE(trig_ids.size() == 2);
  CHECK(trig_ids[0] == s1);
  CHECK(trig_ids[1] == s2);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 3);
  CHECK(ex[0].as<EvExecuted>().price == 2004);
  CHECK(ex[1].as<EvExecuted>().price == 2008);
  CHECK(ex[1].as<EvExecuted>().incoming == s1);
  CHECK(ex[2].as<EvExecuted>().price == 2012);
  CHECK(ex[2].as<EvExecuted>().incoming == s2);
  CHECK(s.eng.book(0).ask.empty());
}

TEST_CASE("stops: hidden midpoint prints evaluate triggers") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  const Seq ask = s.limit(Side::sell, 30, 2004);
  s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0);
  const Seq st = s.enter(Side::buy, OrderKind::stop, Tif::day, 5, 0, 2002);
  s.events();
  // The incoming buy fills entirely against the hidden order at the midpoint.
  s.limit(Side::buy, 10, 2004);
  auto evs = s.events();
  auto tg = filter(evs, EventType::stop_triggered);
  REQUIRE(tg.size() == 1);
  CHECK(tg[0].as<EvStopTriggered>().id == st);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().price == 2002);
  CHECK((ex[0].as<EvExecuted>().flags & EvExecuted::kHidden) != 0);
  CHECK(ex[1].as<EvExecuted>().incoming == st);
  CHECK(ex[1].as<EvExecuted>().resting == ask);
  CHECK(ex[1].as<EvExecuted>().price == 2004);
}

TEST_CASE("stops: stop_limit rests at its limit when unmarketable, market stop cancels") {
  Sess s;
  s.limit(Side::sell, 10, 2000);
  const Seq sl = s.enter(Side::buy, OrderKind::stop_limit, Tif::day, 8, 1996, 2000);
  const Seq sm = s.enter(Side::buy, OrderKind::stop, Tif::day, 50, 0, 2000);
  s.limit(Side::sell, 5, 2004);
  s.events();
  // This print consumes the whole 2000 level and fires both stops.
  s.limit(Side::buy, 10, 2000);
  auto evs = s.events();
  auto tg = filter(evs, EventType::stop_triggered);
  REQUIRE(tg.size() == 2);
  CHECK(tg[0].as<EvStopTriggered>().id == sl);
  CHECK(tg[1].as<EvStopTriggered>().id == sm);
  // Stop limit found nothing at or below 1996 and rests there.
  const Order* o = s.eng.find_order(sl);
  REQUIRE(o != nullptr);
  CHECK(ostate(*o) == OrderState::resting);
  CHECK(o->price == Price{1996});
  CHECK(s.eng.book(0).bid.best() == Price{1996});
  // Market stop fills 5 at 2004 and cancels the rest with no_liquidity.
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == sm);
  CHECK(cx[0].as<EvCanceled>().qty_canceled == 45);
  CHECK(cx[0].as<EvCanceled>().reason == Reason::no_liquidity);
}

TEST_CASE("smp: all three policies on displayed matches, policy from incoming") {
  SUBCASE("cancel_resting cancels the resting order and continues") {
    Sess s;
    const Seq self = s.limit(Side::sell, 50, 2000, Tif::day, 1);
    const Seq other = s.limit(Side::sell, 60, 2000, Tif::day, 2);
    s.events();
    const Seq in = s.limit(Side::buy, 80, 2000, Tif::day, 1, SmpPolicy::cancel_resting);
    auto evs = s.events();
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == self);
    CHECK(cx[0].as<EvCanceled>().qty_canceled == 50);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
    auto ex = filter(evs, EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == other);
    CHECK(ex[0].as<EvExecuted>().qty == 60);
    // Remainder rests as usual.
    const Order* o = s.eng.find_order(in);
    REQUIRE(o != nullptr);
    CHECK(o->display_open == 20);
  }
  SUBCASE("cancel_incoming cancels the incoming remainder, resting intact") {
    Sess s;
    const Seq self = s.limit(Side::sell, 50, 2000, Tif::day, 1);
    s.events();
    const Seq in = s.limit(Side::buy, 80, 2000, Tif::day, 1, SmpPolicy::cancel_incoming);
    auto evs = s.events();
    CHECK(filter(evs, EventType::executed).empty());
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == in);
    CHECK(cx[0].as<EvCanceled>().qty_canceled == 80);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
    REQUIRE(s.eng.find_order(self) != nullptr);
    CHECK(s.eng.find_order(self)->display_open == 50);
    CHECK(s.eng.find_order(in) == nullptr);
  }
  SUBCASE("cancel_both cancels resting and incoming") {
    Sess s;
    const Seq self = s.limit(Side::sell, 50, 2000, Tif::day, 1);
    const Seq other = s.limit(Side::sell, 60, 2000, Tif::day, 2);
    s.events();
    const Seq in = s.limit(Side::buy, 80, 2000, Tif::day, 1, SmpPolicy::cancel_both);
    auto evs = s.events();
    CHECK(filter(evs, EventType::executed).empty());
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 2);
    CHECK(cx[0].as<EvCanceled>().id == self);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
    CHECK(cx[1].as<EvCanceled>().id == in);
    CHECK(cx[1].as<EvCanceled>().reason == Reason::smp);
    CHECK(s.eng.find_order(other) != nullptr);
  }
  SUBCASE("policy is taken from the incoming order only") {
    Sess s;
    // Resting carries cancel_resting but the incoming none policy wins.
    const Seq self = s.limit(Side::sell, 50, 2000, Tif::day, 1, SmpPolicy::cancel_resting);
    s.events();
    s.limit(Side::buy, 50, 2000, Tif::day, 1, SmpPolicy::none);
    auto evs = s.events();
    CHECK(filter(evs, EventType::canceled).empty());
    auto ex = filter(evs, EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == self);
    CHECK(ex[0].as<EvExecuted>().qty == 50);
  }
}

TEST_CASE("smp: applies to hidden midpoint matches") {
  SUBCASE("cancel_resting on a hidden order") {
    Sess s;
    s.limit(Side::buy, 10, 2000, Tif::day, 9);
    const Seq ask = s.limit(Side::sell, 30, 2010, Tif::day, 2);
    const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 0, 0, 1);
    s.events();
    s.limit(Side::buy, 30, 2010, Tif::day, 1, SmpPolicy::cancel_resting);
    auto evs = s.events();
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == hid);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
    auto ex = filter(evs, EventType::executed);
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].as<EvExecuted>().resting == ask);
    CHECK(ex[0].as<EvExecuted>().qty == 30);
  }
  SUBCASE("cancel_incoming against a hidden order") {
    Sess s;
    s.limit(Side::buy, 10, 2000, Tif::day, 9);
    const Seq ask = s.limit(Side::sell, 30, 2010, Tif::day, 2);
    const Seq hid = s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 0, 0, 1);
    s.events();
    const Seq in = s.limit(Side::buy, 30, 2010, Tif::day, 1, SmpPolicy::cancel_incoming);
    auto evs = s.events();
    CHECK(filter(evs, EventType::executed).empty());
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == in);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::smp);
    CHECK(s.eng.find_order(hid) != nullptr);
    CHECK(s.eng.find_order(ask) != nullptr);
  }
}

TEST_CASE("replace: ITCH semantics, new id, time priority loss, old id gone") {
  Sess s;
  const Seq b1 = s.limit(Side::buy, 100, 2000);
  const Seq b2 = s.limit(Side::buy, 50, 2000);
  s.events();
  const Seq rid = s.replace(b1, 100, 2000);
  auto evs = s.events();
  REQUIRE(evs.size() == 2);
  CHECK(evs[0].type == EventType::replaced);
  CHECK(evs[0].as<EvReplaced>().old_id == b1);
  CHECK(evs[0].as<EvReplaced>().new_id == rid);
  CHECK(evs[1].type == EventType::accepted);
  CHECK(evs[1].as<EvAccepted>().id == rid);
  CHECK(s.eng.find_order(b1) == nullptr);
  // The replacement now sits behind b2 even at the same price.
  s.limit(Side::sell, 120, 2000);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().resting == b2);
  CHECK(ex[0].as<EvExecuted>().qty == 50);
  CHECK(ex[1].as<EvExecuted>().resting == rid);
  CHECK(ex[1].as<EvExecuted>().qty == 70);
  // Operations on the dead old id reject.
  s.cancel(b1);
  CHECK(s.lone_reject() == Reason::unknown_target);
  s.replace(b1, 10, 2000);
  CHECK(s.lone_reject() == Reason::unknown_target);
}

TEST_CASE("replace: non limit targets and dead ids reject") {
  Sess s;
  const Seq st = s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2000);
  s.limit(Side::buy, 10, 1990);
  const Seq peg = s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 2);
  s.events();
  s.replace(st, 10, 2000);
  CHECK(s.lone_reject() == Reason::bad_kind);
  s.replace(peg, 10, 2000);
  CHECK(s.lone_reject() == Reason::bad_kind);
  s.replace(99999, 10, 2000);
  CHECK(s.lone_reject() == Reason::unknown_target);
  // Queued pre open market orders also reject.
  Sess p(false);
  const Seq m = p.market(Side::buy, 10);
  p.events();
  p.replace(m, 10, 2000);
  CHECK(p.lone_reject() == Reason::bad_kind);
}

TEST_CASE("replace: allowed in pre open and does not match") {
  Sess s(false);
  const Seq b = s.limit(Side::buy, 100, 2000);
  s.limit(Side::sell, 10, 2002);
  s.events();
  const Seq rid = s.replace(b, 50, 2004);
  auto evs = s.events();
  REQUIRE(evs.size() == 2);
  CHECK(evs[0].type == EventType::replaced);
  CHECK(evs[1].type == EventType::accepted);
  CHECK(evs[1].as<EvAccepted>().id == rid);
  // The book is crossed and stays crossed: no executions pre open.
  CHECK(s.eng.book(0).bid.best() == Price{2004});
  CHECK(s.eng.book(0).ask.best() == Price{2002});
}

TEST_CASE("reduce: keeps queue position, reduce to zero cancels") {
  Sess s;
  const Seq b1 = s.limit(Side::buy, 100, 2000);
  const Seq b2 = s.limit(Side::buy, 50, 2000);
  s.events();
  s.reduce(b1, 40);
  auto rd = filter(s.events(), EventType::reduced);
  REQUIRE(rd.size() == 1);
  CHECK(rd[0].as<EvReduced>().id == b1);
  CHECK(rd[0].as<EvReduced>().qty_reduced == 40);
  CHECK(rd[0].as<EvReduced>().remaining == 60);
  // b1 still fills first: priority kept.
  s.limit(Side::sell, 80, 2000);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().resting == b1);
  CHECK(ex[0].as<EvExecuted>().qty == 60);
  CHECK(ex[1].as<EvExecuted>().resting == b2);
  CHECK(ex[1].as<EvExecuted>().qty == 20);
  // Reduce to exactly zero cancels.
  const Seq b3 = s.limit(Side::buy, 50, 1998);
  s.events();
  s.reduce(b3, 50);
  auto cx = filter(s.events(), EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == b3);
  CHECK(cx[0].as<EvCanceled>().qty_canceled == 50);
  CHECK(s.eng.find_order(b3) == nullptr);
  // Reduce by more than open also cancels.
  const Seq b4 = s.limit(Side::buy, 50, 1998);
  s.events();
  s.reduce(b4, 60);
  cx = filter(s.events(), EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == b4);
  CHECK(s.eng.find_order(b4) == nullptr);
  // Bad reduce inputs reject.
  const Seq b5 = s.limit(Side::buy, 50, 1998);
  s.events();
  s.reduce(b5, 0);
  CHECK(s.lone_reject() == Reason::bad_qty);
  s.reduce(424242, 10);
  CHECK(s.lone_reject() == Reason::unknown_target);
}

TEST_CASE("sessions: entry rejected in halted, post_close and ended") {
  Engine e(cfg());
  Seq q = 0;
  Ts t = 0;
  e.on_msg(make_enter(++q, ++t, 0, Side::buy, OrderKind::limit, Tif::day, 10, 2000));
  auto evs = parse_events(e.emitter().buffer());
  e.emitter().drain();
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].type == EventType::rejected);
  CHECK(evs[0].as<EvRejected>().reason == Reason::wrong_state);
  // Cancels also reject outside pre open and open.
  e.on_msg(make_cancel(++q, ++t, 1));
  evs = parse_events(e.emitter().buffer());
  e.emitter().drain();
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].as<EvRejected>().reason == Reason::wrong_state);

  Sess s;
  s.clock(ClockAction::close_cross);
  s.events();
  CHECK(s.eng.state() == SessionState::post_close);
  s.limit(Side::buy, 10, 2000);
  CHECK(s.lone_reject() == Reason::wrong_state);
  s.clock(ClockAction::session_end);
  s.events();
  CHECK(s.eng.state() == SessionState::ended);
  s.limit(Side::buy, 10, 2000);
  CHECK(s.lone_reject() == Reason::wrong_state);
}

TEST_CASE("pre open: accepts without matching, rejects ioc and fok, queues markets") {
  Sess s(false);
  const Seq b = s.limit(Side::buy, 100, 2010);
  const Seq a = s.limit(Side::sell, 100, 2000);
  auto evs = s.events();
  CHECK(filter(evs, EventType::accepted).size() == 2);
  CHECK(filter(evs, EventType::executed).empty());
  // Crossed book is allowed pre open.
  CHECK(s.eng.book(0).bid.best() == Price{2010});
  CHECK(s.eng.book(0).ask.best() == Price{2000});
  CHECK(s.eng.find_order(b)->display_open == 100);
  CHECK(s.eng.find_order(a)->display_open == 100);
  s.limit(Side::buy, 10, 2000, Tif::ioc);
  CHECK(s.lone_reject() == Reason::tif_in_auction);
  s.limit(Side::buy, 10, 2000, Tif::fok);
  CHECK(s.lone_reject() == Reason::tif_in_auction);
  // Market orders queue per side, outside the ladder.
  const Seq m = s.market(Side::buy, 30);
  evs = s.events();
  REQUIRE(filter(evs, EventType::accepted).size() == 1);
  CHECK(filter(evs, EventType::accepted)[0].as<EvAccepted>().display == 0);
  const Order* o = s.eng.find_order(m);
  REQUIRE(o != nullptr);
  CHECK(ostate(*o) == OrderState::queued_market);
  // Stops are accepted pre open.
  const Seq st = s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2400);
  s.events();
  REQUIRE(s.eng.find_order(st) != nullptr);
  CHECK(ostate(*s.eng.find_order(st)) == OrderState::pending_stop);
}

TEST_CASE("open cross: volume maximizing price and full allocation order") {
  Sess s(false);
  const Seq mkt = s.market(Side::buy, 30);
  const Seq b2004 = s.limit(Side::buy, 50, 2004);
  const Seq b2002 = s.limit(Side::buy, 40, 2002);
  const Seq s2000 = s.limit(Side::sell, 100, 2000);
  const Seq s2002 = s.limit(Side::sell, 60, 2002);
  s.events();
  s.clock(ClockAction::open_cross);
  auto evs = s.events();
  CHECK(s.eng.state() == SessionState::open);
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 4);
  // Market first, then better priced limits, then limits at the cross price.
  auto e0 = ex[0].as<EvExecuted>();
  CHECK(e0.resting == s2000);
  CHECK(e0.incoming == mkt);
  CHECK(e0.qty == 30);
  CHECK(e0.price == 2002);
  CHECK(e0.resting_remaining == 70);
  CHECK((e0.flags & EvExecuted::kAuction) != 0);
  auto e1 = ex[1].as<EvExecuted>();
  CHECK(e1.resting == s2000);
  CHECK(e1.incoming == b2004);
  CHECK(e1.qty == 50);
  CHECK(e1.resting_remaining == 20);
  auto e2 = ex[2].as<EvExecuted>();
  CHECK(e2.resting == s2000);
  CHECK(e2.incoming == b2002);
  CHECK(e2.qty == 20);
  CHECK(e2.resting_remaining == 0);
  // Boundary order at the cross price is filled partially.
  auto e3 = ex[3].as<EvExecuted>();
  CHECK(e3.resting == s2002);
  CHECK(e3.incoming == b2002);
  CHECK(e3.qty == 20);
  CHECK(e3.resting_remaining == 40);
  CHECK((e3.flags & EvExecuted::kAuction) != 0);
  for (const auto& e : ex) CHECK(e.as<EvExecuted>().price == 2002);
  auto ar = filter(evs, EventType::auction_result);
  REQUIRE(ar.size() == 1);
  auto r = ar[0].as<EvAuctionResult>();
  CHECK(r.price == 2002);
  CHECK(r.matched_qty == 120);
  CHECK(r.imbalance == 40);
  CHECK(r.symbol == 0);
  // No market remainder: nothing canceled.
  CHECK(filter(evs, EventType::canceled).empty());
  CHECK(s.eng.book(0).bid.empty());
  CHECK(s.eng.book(0).ask.best() == Price{2002});
  CHECK(s.eng.find_order(s2002)->display_open == 40);
  CHECK(s.eng.book(0).last_trade == Price{2002});
}

TEST_CASE("open cross: market FIFO and auction_unfilled remainder") {
  Sess s(false);
  const Seq m1 = s.market(Side::buy, 20);
  const Seq m2 = s.market(Side::buy, 30);
  const Seq sl = s.limit(Side::sell, 40, 2000);
  const Seq bl = s.limit(Side::buy, 10, 2000);
  s.events();
  s.clock(ClockAction::open_cross);
  auto evs = s.events();
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().incoming == m1);
  CHECK(ex[0].as<EvExecuted>().qty == 20);
  CHECK(ex[1].as<EvExecuted>().incoming == m2);
  CHECK(ex[1].as<EvExecuted>().qty == 20);
  CHECK(ex[1].as<EvExecuted>().resting == sl);
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == 1);
  CHECK(cx[0].as<EvCanceled>().id == m2);
  CHECK(cx[0].as<EvCanceled>().qty_canceled == 10);
  CHECK(cx[0].as<EvCanceled>().reason == Reason::auction_unfilled);
  auto r = filter(evs, EventType::auction_result)[0].as<EvAuctionResult>();
  CHECK(r.price == 2000);
  CHECK(r.matched_qty == 40);
  CHECK(r.imbalance == 20);
  // The unfilled at price limit buy stays in the open book.
  REQUIRE(s.eng.find_order(bl) != nullptr);
  CHECK(s.eng.find_order(bl)->display_open == 10);
}

TEST_CASE("open cross: FIFO within a price class") {
  Sess s(false);
  const Seq s1 = s.limit(Side::sell, 30, 2000);
  const Seq s2 = s.limit(Side::sell, 30, 2000);
  s.market(Side::buy, 40);
  s.events();
  s.clock(ClockAction::open_cross);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 2);
  CHECK(ex[0].as<EvExecuted>().resting == s1);
  CHECK(ex[0].as<EvExecuted>().qty == 30);
  CHECK(ex[1].as<EvExecuted>().resting == s2);
  CHECK(ex[1].as<EvExecuted>().qty == 10);
  CHECK(ex[1].as<EvExecuted>().resting_remaining == 20);
  REQUIRE(s.eng.find_order(s2) != nullptr);
  CHECK(s.eng.find_order(s2)->display_open == 20);
}

TEST_CASE("open cross: tie break by imbalance at equal volume") {
  Sess s(false);
  s.limit(Side::buy, 100, 2002);
  s.limit(Side::buy, 60, 2000);
  s.limit(Side::sell, 100, 2000);
  s.limit(Side::sell, 50, 2002);
  s.events();
  s.clock(ClockAction::open_cross);
  // Volume ties at 100 for 2000 and 2002; imbalance 60 vs 50 chooses 2002.
  auto r = filter(s.events(), EventType::auction_result)[0].as<EvAuctionResult>();
  CHECK(r.matched_qty == 100);
  CHECK(r.price == 2002);
  CHECK(r.imbalance == 50);
}

namespace {

// Builds a pre open book whose cross volume and imbalance tie at 2000 and 2002.
std::vector<std::uint8_t> tied_preopen_snapshot() {
  Sess s(false);
  s.limit(Side::buy, 100, 2002);
  s.limit(Side::buy, 50, 2000);
  s.limit(Side::sell, 100, 2000);
  s.limit(Side::sell, 50, 2002);
  std::vector<std::uint8_t> snap;
  s.eng.save_snapshot(snap);
  return snap;
}

// Patches last_trade for symbol 0 so the cross has a reference price.
void patch_last_trade(std::vector<std::uint8_t>& snap, std::int32_t v) {
  const std::size_t off = 80 + 8;
  REQUIRE(snap.size() > off + 4);
  std::int32_t cur = -1;
  std::memcpy(&cur, snap.data() + off, 4);
  REQUIRE(cur == 0);
  std::memcpy(snap.data() + off, &v, 4);
}

std::int32_t cross_price_with_ref(std::int32_t ref) {
  auto snap = tied_preopen_snapshot();
  if (ref != 0) patch_last_trade(snap, ref);
  Engine e(cfg());
  e.load_snapshot(snap.data(), snap.size());
  e.on_msg(make_clock(e.last_seq() + 1, 100, ClockAction::open_cross));
  auto ar = filter(parse_events(e.emitter().buffer()), EventType::auction_result);
  REQUIRE(ar.size() == 1);
  CHECK(ar[0].as<EvAuctionResult>().matched_qty == 100);
  return ar[0].as<EvAuctionResult>().price;
}

}  // namespace

TEST_CASE("open cross: tie break by reference proximity then lower price") {
  // Reference at 2002 selects the closer candidate 2002.
  CHECK(cross_price_with_ref(2002) == 2002);
  // Equidistant reference falls through to the lower price.
  CHECK(cross_price_with_ref(2001) == 2000);
  // No reference at all also takes the lower price.
  CHECK(cross_price_with_ref(0) == 2000);
}

TEST_CASE("open cross: no overlap emits zero result and cancels market orders") {
  SUBCASE("limits only, no overlap") {
    Sess s(false);
    const Seq b = s.limit(Side::buy, 10, 1990);
    const Seq a = s.limit(Side::sell, 10, 2010);
    s.events();
    s.clock(ClockAction::open_cross);
    auto evs = s.events();
    CHECK(filter(evs, EventType::executed).empty());
    CHECK(filter(evs, EventType::canceled).empty());
    auto r = filter(evs, EventType::auction_result)[0].as<EvAuctionResult>();
    CHECK(r.price == 0);
    CHECK(r.matched_qty == 0);
    CHECK(s.eng.state() == SessionState::open);
    CHECK(s.eng.find_order(b) != nullptr);
    CHECK(s.eng.find_order(a) != nullptr);
  }
  SUBCASE("market order with an empty far side cancels auction_unfilled") {
    Sess s(false);
    const Seq m = s.market(Side::buy, 5);
    const Seq b = s.limit(Side::buy, 10, 1990);
    s.events();
    s.clock(ClockAction::open_cross);
    auto evs = s.events();
    auto r = filter(evs, EventType::auction_result)[0].as<EvAuctionResult>();
    CHECK(r.price == 0);
    CHECK(r.matched_qty == 0);
    auto cx = filter(evs, EventType::canceled);
    REQUIRE(cx.size() == 1);
    CHECK(cx[0].as<EvCanceled>().id == m);
    CHECK(cx[0].as<EvCanceled>().reason == Reason::auction_unfilled);
    CHECK(s.eng.find_order(b) != nullptr);
    CHECK(s.eng.state() == SessionState::open);
  }
}

TEST_CASE("open cross: AuctionResult per active symbol in symbol index order") {
  Sess s(false, 3);
  // Symbols 2 and 0 are active, symbol 1 stays empty.
  s.enter(Side::buy, OrderKind::limit, Tif::day, 10, 2000, 0, 0, SmpPolicy::none, 2);
  s.enter(Side::sell, OrderKind::limit, Tif::day, 10, 2000, 0, 0, SmpPolicy::none, 2);
  s.enter(Side::buy, OrderKind::limit, Tif::day, 5, 3000, 0, 0, SmpPolicy::none, 0);
  s.events();
  s.clock(ClockAction::open_cross);
  auto ar = filter(s.events(), EventType::auction_result);
  REQUIRE(ar.size() == 2);
  CHECK(ar[0].as<EvAuctionResult>().symbol == 0);
  CHECK(ar[0].as<EvAuctionResult>().matched_qty == 0);
  CHECK(ar[1].as<EvAuctionResult>().symbol == 2);
  CHECK(ar[1].as<EvAuctionResult>().matched_qty == 10);
  CHECK(ar[1].as<EvAuctionResult>().price == 2000);
}

TEST_CASE("open cross: stops do not participate and trigger after as one batch") {
  Sess s(false);
  const Seq b = s.limit(Side::buy, 50, 2000);
  const Seq a = s.limit(Side::sell, 50, 2000);
  const Seq far = s.limit(Side::sell, 30, 2004);
  const Seq st = s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2000);
  s.events();
  s.clock(ClockAction::open_cross);
  auto evs = s.events();
  auto ex = filter(evs, EventType::executed);
  REQUIRE(ex.size() == 2);
  // Auction matched only the limits; the stop fill follows outside the cross.
  auto e0 = ex[0].as<EvExecuted>();
  CHECK(e0.resting == a);
  CHECK(e0.incoming == b);
  CHECK(e0.qty == 50);
  CHECK((e0.flags & EvExecuted::kAuction) != 0);
  auto e1 = ex[1].as<EvExecuted>();
  CHECK(e1.resting == far);
  CHECK(e1.incoming == st);
  CHECK(e1.qty == 10);
  CHECK(e1.price == 2004);
  CHECK((e1.flags & EvExecuted::kAuction) == 0);
  // The trigger conversion comes after the AuctionResult.
  std::size_t i_result = 0, i_trig = 0;
  for (std::size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].type == EventType::auction_result) i_result = i;
    if (evs[i].type == EventType::stop_triggered) i_trig = i;
  }
  REQUIRE(filter(evs, EventType::stop_triggered).size() == 1);
  CHECK(i_trig > i_result);
}

TEST_CASE("close cross: cancels every live order with session_end in id order") {
  Sess s;
  std::set<OrderId> live;
  live.insert(s.limit(Side::buy, 10, 2000));
  live.insert(s.limit(Side::sell, 10, 2010));
  live.insert(s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0));
  live.insert(s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2400));
  live.insert(s.enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 2));
  live.insert(s.iceberg(Side::sell, 40, 2012, 5));
  s.events();
  s.clock(ClockAction::close_cross);
  auto evs = s.events();
  CHECK(s.eng.state() == SessionState::post_close);
  auto cx = filter(evs, EventType::canceled);
  REQUIRE(cx.size() == live.size());
  OrderId prev = 0;
  std::set<OrderId> seen;
  for (const auto& e : cx) {
    auto c = e.as<EvCanceled>();
    CHECK(c.reason == Reason::session_end);
    CHECK(c.id > prev);
    prev = c.id;
    seen.insert(c.id);
  }
  CHECK(seen == live);
  // Activity was nonzero so an AuctionResult is emitted, with no volume.
  auto ar = filter(evs, EventType::auction_result);
  REQUIRE(ar.size() == 1);
  CHECK(ar[0].as<EvAuctionResult>().matched_qty == 0);
  CHECK(s.eng.book(0).bid.empty());
  CHECK(s.eng.book(0).ask.empty());
}

TEST_CASE("close cross: SessionEvent follows the session_end cancels") {
  Sess s;
  s.limit(Side::buy, 10, 2000);
  s.events();
  s.clock(ClockAction::close_cross);
  auto evs = s.events();
  std::ptrdiff_t i_sess = -1, i_last_cancel = -1;
  for (std::size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].type == EventType::session_event &&
        evs[i].as<EvSessionEvent>().action == ClockAction::close_cross)
      i_sess = static_cast<std::ptrdiff_t>(i);
    if (evs[i].type == EventType::canceled &&
        evs[i].as<EvCanceled>().reason == Reason::session_end)
      i_last_cancel = static_cast<std::ptrdiff_t>(i);
  }
  REQUIRE(i_sess >= 0);
  REQUIRE(i_last_cancel >= 0);
  // Doc: cancel in id order, then emit SessionEvent.
  CHECK(i_sess > i_last_cancel);
}

TEST_CASE("sessions: post close to ended and clock events in wrong states reject") {
  Sess s;
  s.clock(ClockAction::close_cross);
  s.events();
  s.clock(ClockAction::session_end);
  auto evs = s.events();
  auto se = filter(evs, EventType::session_event);
  REQUIRE(se.size() == 1);
  CHECK(se[0].as<EvSessionEvent>().action == ClockAction::session_end);
  CHECK(s.eng.state() == SessionState::ended);

  Engine e(cfg());
  Seq q = 0;
  Ts t = 0;
  auto expect_reject = [&](ClockAction a) {
    e.on_msg(make_clock(++q, ++t, a));
    auto v = parse_events(e.emitter().buffer());
    e.emitter().drain();
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].type == EventType::rejected);
    CHECK(v[0].as<EvRejected>().reason == Reason::wrong_state);
  };
  // Halted accepts only session_start.
  expect_reject(ClockAction::open_cross);
  expect_reject(ClockAction::close_cross);
  expect_reject(ClockAction::session_end);
  e.on_msg(make_clock(++q, ++t, ClockAction::session_start));
  e.emitter().drain();
  CHECK(e.state() == SessionState::pre_open);
  // Pre open accepts only open_cross.
  expect_reject(ClockAction::session_start);
  expect_reject(ClockAction::close_cross);
  expect_reject(ClockAction::session_end);
  e.on_msg(make_clock(++q, ++t, ClockAction::open_cross));
  e.emitter().drain();
  CHECK(e.state() == SessionState::open);
  // Open accepts only close_cross.
  expect_reject(ClockAction::session_start);
  expect_reject(ClockAction::open_cross);
  expect_reject(ClockAction::session_end);
}

TEST_CASE("collar and ladder: reanchor when possible, collar cancel when not") {
  Sess s;
  const Seq near = s.limit(Side::buy, 10, 2000);
  s.events();
  // 18000 cannot fit in a window that still holds 2000.
  const Seq far = s.limit(Side::buy, 10, 18000);
  auto evs = s.events();
  REQUIRE(evs.size() == 2);
  CHECK(evs[0].type == EventType::accepted);
  CHECK(evs[1].type == EventType::canceled);
  auto c = evs[1].as<EvCanceled>();
  CHECK(c.id == far);
  CHECK(c.qty_canceled == 10);
  CHECK(c.reason == Reason::collar);
  CHECK(s.eng.find_order(far) == nullptr);
  CHECK(s.eng.book(0).bid.best() == Price{2000});
  // After the blocking order leaves, the same entry reanchors and rests.
  s.cancel(near);
  s.events();
  const Seq far2 = s.limit(Side::buy, 10, 18000);
  evs = s.events();
  CHECK(filter(evs, EventType::canceled).empty());
  REQUIRE(s.eng.find_order(far2) != nullptr);
  CHECK(s.eng.book(0).bid.best() == Price{18000});
}

namespace {

// Scripted run exercising pegs, stops, icebergs, hidden mids and an auction.
std::pair<std::uint64_t, std::vector<std::uint8_t>> deterministic_run() {
  Engine e(cfg(1, 7));
  Seq q = 0;
  Ts t = 0;
  auto enter = [&](Side sd, OrderKind k, Tif tif, Qty qty, std::int32_t px,
                   std::int32_t aux = 0, Participant part = 0,
                   SmpPolicy smp = SmpPolicy::none) {
    e.on_msg(make_enter(++q, ++t, 0, sd, k, tif, qty, px, aux, part, smp));
    return q;
  };
  e.on_msg(make_clock(++q, ++t, ClockAction::session_start));
  enter(Side::buy, OrderKind::limit, Tif::day, 100, 2000);
  enter(Side::sell, OrderKind::limit, Tif::day, 80, 2000);
  const Seq b1996 = enter(Side::buy, OrderKind::limit, Tif::day, 40, 1996);
  enter(Side::buy, OrderKind::market, Tif::day, 30, 0);
  enter(Side::buy, OrderKind::stop, Tif::day, 25, 0, 2000);
  enter(Side::sell, OrderKind::stop, Tif::day, 10, 0, 1700);
  e.on_msg(make_clock(++q, ++t, ClockAction::open_cross));
  const Seq ice = enter(Side::sell, OrderKind::limit, Tif::day, 60, 2004, 10);
  enter(Side::sell, OrderKind::peg_mid, Tif::day, 20, 0);
  const Seq pegb = enter(Side::buy, OrderKind::peg_primary, Tif::day, 10, 0, 2);
  enter(Side::sell, OrderKind::peg_market, Tif::day, 15, 0, 2);
  enter(Side::buy, OrderKind::limit, Tif::day, 30, 2004, 0, 3, SmpPolicy::cancel_resting);
  e.on_msg(make_reduce(++q, ++t, ice, 25));
  e.on_msg(make_replace(++q, ++t, b1996, 50, 1998));
  enter(Side::sell, OrderKind::limit, Tif::day, 35, 1998);
  e.on_msg(make_cancel(++q, ++t, pegb));
  enter(Side::buy, OrderKind::limit, Tif::ioc, 100, 2004);
  enter(Side::sell, OrderKind::limit, Tif::fok, 500, 1900);
  e.on_msg(make_clock(++q, ++t, ClockAction::close_cross));
  e.on_msg(make_clock(++q, ++t, ClockAction::session_end));
  return {e.emitter().hash(), e.emitter().buffer()};
}

}  // namespace

TEST_CASE("determinism: identical runs produce identical bytes and hashes") {
  const auto a = deterministic_run();
  const auto b = deterministic_run();
  CHECK(a.first == b.first);
  CHECK(a.second == b.second);
  CHECK_FALSE(a.second.empty());
  // The run includes hidden and auction executions plus stop and peg activity.
  auto evs = parse_events(a.second);
  bool saw_hidden = false, saw_auction = false;
  for (const auto& e : filter(evs, EventType::executed)) {
    const auto x = e.as<EvExecuted>();
    if (x.flags & EvExecuted::kHidden) saw_hidden = true;
    if (x.flags & EvExecuted::kAuction) saw_auction = true;
  }
  CHECK(saw_hidden);
  CHECK(saw_auction);
  CHECK_FALSE(filter(evs, EventType::stop_triggered).empty());
  CHECK_FALSE(filter(evs, EventType::repriced).empty());
  CHECK_FALSE(filter(evs, EventType::refilled).empty());
  CHECK_FALSE(filter(evs, EventType::state_hash).empty());
}

TEST_CASE("determinism: StateHash at the configured checkpoint interval") {
  Sess s(true, 1, 5);
  // The session setup consumed events 1 and 2.
  for (int i = 0; i < 8; ++i)
    s.limit(Side::buy, 10, 1000 + 2 * i);
  auto sh = filter(s.events(), EventType::state_hash);
  REQUIRE(sh.size() == 2);
  auto h0 = sh[0].as<EvStateHash>();
  auto h1 = sh[1].as<EvStateHash>();
  CHECK(h0.events_processed == 5);
  CHECK(h1.events_processed == 10);
  CHECK(h1.book_digest == s.eng.book_digest());
  CHECK(h0.out_hash != h1.out_hash);
}

TEST_CASE("output integrity: executions reference previously accepted ids") {
  Sess s(false);
  s.market(Side::buy, 20);
  s.limit(Side::buy, 50, 2002);
  s.limit(Side::sell, 60, 2000);
  s.enter(Side::buy, OrderKind::stop, Tif::day, 10, 0, 2002);
  s.clock(ClockAction::open_cross);
  s.limit(Side::buy, 10, 2000);
  s.limit(Side::sell, 30, 2002);
  s.enter(Side::sell, OrderKind::peg_mid, Tif::day, 10, 0);
  s.limit(Side::buy, 25, 2002);
  s.market(Side::sell, 15);
  std::set<OrderId> accepted;
  for (const auto& e : s.events()) {
    if (e.type == EventType::accepted) accepted.insert(e.as<EvAccepted>().id);
    if (e.type == EventType::executed) {
      const auto x = e.as<EvExecuted>();
      CHECK(accepted.count(x.resting) == 1);
      CHECK(accepted.count(x.incoming) == 1);
      CHECK(x.qty > 0);
      CHECK(x.resting_remaining >= 0);
    }
    if (e.type == EventType::stop_triggered)
      CHECK(accepted.count(e.as<EvStopTriggered>().id) == 1);
  }
}

TEST_CASE("output integrity: resting_remaining on partial fills") {
  Sess s;
  const Seq a = s.limit(Side::sell, 100, 2000);
  s.events();
  s.limit(Side::buy, 30, 2000);
  auto ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().resting == a);
  CHECK(ex[0].as<EvExecuted>().resting_remaining == 70);
  s.limit(Side::buy, 45, 2000);
  ex = filter(s.events(), EventType::executed);
  REQUIRE(ex.size() == 1);
  CHECK(ex[0].as<EvExecuted>().resting_remaining == 25);
  CHECK(s.eng.find_order(a)->display_open == 25);
}
