#include "doctest.h"
#include "gavel/engine.hpp"

using namespace gavel;

namespace {
struct Sess {
  Engine eng;
  Seq seq{0};
  Ts ts{0};
  explicit Sess(std::uint32_t symbols = 1) : eng(Config{symbols, 8192, 1 << 12, 1u << 26, 0}) {
    eng.on_msg(make_clock(++seq, ++ts, ClockAction::session_start));
    eng.on_msg(make_clock(++seq, ++ts, ClockAction::open_cross));
  }
  Seq limit(Side s, Qty q, std::int32_t ticks, Tif tif = Tif::day) {
    eng.on_msg(make_enter(++seq, ++ts, 0, s, OrderKind::limit, tif, q, ticks * 2));
    return seq;
  }
  Seq market(Side s, Qty q) {
    eng.on_msg(make_enter(++seq, ++ts, 0, s, OrderKind::market, Tif::day, q, 0));
    return seq;
  }
};
}  // namespace

TEST_CASE("resting and matching at price time priority") {
  Sess s;
  const Seq b1 = s.limit(Side::buy, 100, 1000);
  const Seq b2 = s.limit(Side::buy, 50, 1000);
  s.limit(Side::buy, 75, 999);
  CHECK(s.eng.book(0).bid.best() == Price::from_ticks(1000));
  // Sell 120 sweeps b1 fully and 20 from b2.
  s.limit(Side::sell, 120, 1000);
  const Order* o1 = s.eng.find_order(b1);
  const Order* o2 = s.eng.find_order(b2);
  CHECK(o1 == nullptr);
  REQUIRE(o2 != nullptr);
  CHECK(o2->display_open == 30);
  CHECK(s.eng.book(0).last_trade == Price::from_ticks(1000));
}

TEST_CASE("market order consumes best then cancels remainder") {
  Sess s;
  s.limit(Side::sell, 10, 1001);
  s.limit(Side::sell, 10, 1002);
  s.market(Side::buy, 25);
  CHECK(s.eng.book(0).ask.empty());
}

TEST_CASE("determinism: identical stream, identical hash") {
  auto run = [] {
    Sess s;
    for (int i = 0; i < 200; ++i) {
      s.limit(i % 2 ? Side::buy : Side::sell, 10 + i % 7, 1000 + (i % 5) - 2);
      if (i % 11 == 0) s.market(Side::buy, 5);
    }
    return s.eng.emitter().hash();
  };
  CHECK(run() == run());
}

TEST_CASE("snapshot restore continues identically") {
  Sess a;
  for (int i = 0; i < 50; ++i) a.limit(i % 2 ? Side::buy : Side::sell, 10, 1000 + i % 3);
  std::vector<std::uint8_t> snap;
  a.eng.save_snapshot(snap);
  Sess b;
  // Fresh engine with same config; replay tail on both and compare.
  Engine restored(Config{1, 8192, 1 << 12, 1u << 26, 0});
  restored.load_snapshot(snap.data(), snap.size());
  CHECK(restored.book_digest() == a.eng.book_digest());
  auto tail = [](Engine& e, Seq seq0) {
    Seq seq = seq0;
    Ts ts = 1000;
    for (int i = 0; i < 30; ++i)
      e.on_msg(make_enter(++seq, ++ts, 0, Side::sell, OrderKind::limit, Tif::day, 8, 2000));
    return e.emitter().hash();
  };
  const Seq s0 = a.seq + 100;
  CHECK(tail(a.eng, s0) == tail(restored, s0));
}

TEST_CASE("auction cross maximizes volume") {
  Engine e(Config{1, 8192, 1 << 12, 1u << 26, 0});
  Seq seq = 0;
  Ts ts = 0;
  e.on_msg(make_clock(++seq, ++ts, ClockAction::session_start));
  e.on_msg(make_enter(++seq, ++ts, 0, Side::buy, OrderKind::limit, Tif::day, 100, 2004));
  e.on_msg(make_enter(++seq, ++ts, 0, Side::buy, OrderKind::limit, Tif::day, 50, 2002));
  e.on_msg(make_enter(++seq, ++ts, 0, Side::sell, OrderKind::limit, Tif::day, 80, 2000));
  e.on_msg(make_enter(++seq, ++ts, 0, Side::sell, OrderKind::limit, Tif::day, 60, 2002));
  e.on_msg(make_clock(++seq, ++ts, ClockAction::open_cross));
  // Volume maximizing price is 2002 half ticks: B(2002)=150, S(2002)=140 -> exec 140.
  CHECK(e.book(0).last_trade == Price{2002});
  CHECK(e.state() == SessionState::open);
}

TEST_CASE("iceberg refills at tail") {
  Sess s;
  const Seq ice = s.eng.last_seq() + 1;
  s.eng.on_msg(make_enter(ice, 100, 0, Side::sell, OrderKind::limit, Tif::day, 100, 2000, 30));
  s.seq = ice;
  s.limit(Side::buy, 30, 1000);
  const Order* o = s.eng.find_order(ice);
  REQUIRE(o != nullptr);
  CHECK(o->display_open == 30);
  CHECK(o->reserve == 40);
  CHECK(o->cum_executed == 30);
}
