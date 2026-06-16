// Deterministic simulation tests; known engine bug repros live under repros/.
#include <cstdint>
#include <string>
#include <vector>
#include "doctest.h"
#include "gavel/engine.hpp"
#include "gavel/verify/generator.hpp"
#include "gavel/verify/invariants.hpp"
#include "gavel/verify/shadow.hpp"

using namespace gavel;

namespace {

Config engine_config(const sim::GenConfig& gc) {
  Config c;
  c.num_symbols = gc.num_symbols;
  c.ladder_window = 8192;
  c.initial_order_capacity = 1u << 12;
  c.max_orders = 1u << 26;
  c.checkpoint_interval = 500;
  return c;
}

struct Outcome {
  std::vector<std::string> violations;
  std::uint64_t hash{0};
  std::uint64_t digest{0};
  std::size_t max_live{0};
};

Outcome run_dst(const std::vector<InputMsg>& msgs, const Config& cfg,
                std::uint64_t check_every) {
  Engine eng(cfg);
  verify::ShadowView shadow;
  Outcome out;
  std::size_t shadow_taken = 0;
  std::size_t i = 0;
  auto inspect = [&]() {
    const auto& buf = eng.emitter().buffer();
    shadow.consume(buf.data(), buf.size());
    eng.emitter().drain();
    for (auto& s : verify::check_invariants(eng))
      out.violations.push_back("invariant after msg " + std::to_string(i) + ": " + s);
    while (shadow_taken < shadow.violations().size())
      out.violations.push_back("shadow: " + shadow.violations()[shadow_taken++]);
    return !out.violations.empty();
  };
  for (const InputMsg& m : msgs) {
    eng.on_msg(m);
    ++i;
    if (i % check_every == 0 && inspect()) break;
  }
  if (out.violations.empty()) inspect();
  out.hash = eng.emitter().hash();
  out.digest = eng.book_digest();
  out.max_live = shadow.max_live();
  return out;
}

std::string head(const Outcome& o) {
  std::string s;
  for (std::size_t k = 0; k < o.violations.size() && k < 3; ++k) s += o.violations[k] + " | ";
  return s;
}

}  // namespace

// 21 of 24 combinations currently fail through the documented zombie stop bug.
TEST_CASE("dst: every preset and seed runs clean" * doctest::may_fail()) {
  for (const std::string preset : {"default", "peg_heavy", "stop_cascade", "iceberg_sweep",
                                   "auction_mix", "tight_book"}) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
      INFO("preset=", preset, " seed=", seed);
      const sim::GenConfig gc = sim::preset_config(preset);
      const auto msgs = sim::Generator(seed, gc).generate(20000);
      const Outcome o = run_dst(msgs, engine_config(gc), 50);
      CHECK_MESSAGE(o.violations.empty(), head(o));
      CHECK(o.max_live > 0);
    }
  }
}

// Stops disabled: the zombie path cannot fire, everything else must hold strictly.
TEST_CASE("dst: presets without stops run clean") {
  for (const std::string preset : {"default", "peg_heavy", "iceberg_sweep", "auction_mix",
                                   "tight_book"}) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
      INFO("preset=", preset, " seed=", seed);
      sim::GenConfig gc = sim::preset_config(preset);
      gc.w_stop = 0;
      gc.w_stop_limit = 0;
      const auto msgs = sim::Generator(seed, gc).generate(20000);
      const Outcome o = run_dst(msgs, engine_config(gc), 50);
      CHECK_MESSAGE(o.violations.empty(), head(o));
      CHECK(o.max_live > 0);
    }
  }
}

// Minimal regression for the zombie stop bug, mirrors repros/repro_default_seed1.stream.
TEST_CASE("dst: triggered market stop with empty opposite book leaves no zombie" *
          doctest::may_fail()) {
  sim::GenConfig gc;
  const Config cfg = engine_config(gc);
  Engine eng(cfg);
  eng.on_msg(make_clock(1, 1, ClockAction::session_start));
  eng.on_msg(make_clock(2, 2, ClockAction::open_cross));
  eng.on_msg(make_enter(3, 3, 0, Side::sell, OrderKind::limit, Tif::day, 100, 1992));
  eng.on_msg(make_enter(4, 4, 0, Side::sell, OrderKind::stop, Tif::day, 50, 0, 2002));
  // Print at 1992 triggers the sell stop; it converts to market with no bids.
  eng.on_msg(make_enter(5, 5, 0, Side::buy, OrderKind::market, Tif::day, 10, 0));
  // The stop must be fully dead now: id 4 unresolvable, cancel rejected.
  CHECK(eng.find_order(4) == nullptr);
  eng.on_msg(make_cancel(6, 6, 4));
  CHECK_MESSAGE(verify::check_invariants(eng).empty(),
                "digest corrupted by cancel of leaked stop order");
}

TEST_CASE("dst: identical input gives identical hash regardless of check cadence") {
  const sim::GenConfig gc = sim::preset_config("peg_heavy");
  const auto msgs = sim::Generator(1, gc).generate(20000);
  const Outcome a = run_dst(msgs, engine_config(gc), 50);
  const Outcome b = run_dst(msgs, engine_config(gc), 7);
  CHECK_MESSAGE(a.violations.empty(), head(a));
  CHECK_MESSAGE(b.violations.empty(), head(b));
  CHECK(a.hash == b.hash);
  CHECK(a.digest == b.digest);
}

TEST_CASE("dst: snapshot restore plus tail replay equals uninterrupted run") {
  const sim::GenConfig gc = sim::preset_config("auction_mix");
  const auto msgs = sim::Generator(2, gc).generate(20000);
  const Config cfg = engine_config(gc);
  Engine a(cfg);
  const std::size_t mid = msgs.size() / 2;
  for (std::size_t i = 0; i < mid; ++i) a.on_msg(msgs[i]);
  // load_snapshot aborts on a corrupted digest, so require a healthy midpoint first.
  REQUIRE(verify::check_invariants(a).empty());
  std::vector<std::uint8_t> snap;
  a.save_snapshot(snap);
  const std::uint64_t a_mid_digest = a.book_digest();
  for (std::size_t i = mid; i < msgs.size(); ++i) a.on_msg(msgs[i]);
  Engine b(cfg);
  b.load_snapshot(snap.data(), snap.size());
  CHECK(b.book_digest() == a_mid_digest);
  CHECK(verify::check_invariants(b).empty());
  for (std::size_t i = mid; i < msgs.size(); ++i) b.on_msg(msgs[i]);
  CHECK(a.emitter().hash() == b.emitter().hash());
  CHECK(a.book_digest() == b.book_digest());
  CHECK(verify::check_invariants(a).empty());
  CHECK(verify::check_invariants(b).empty());
}
