#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "gavel/engine.hpp"
#include "gavel/itch/itch.hpp"
#include "gavel/stream.hpp"

// Replays an extracted stream against gavel and validates venue executions.
namespace {

// Stream seqs are shifted left so counterfactual injections fit between records.
constexpr unsigned kShift = 20;

using Lvl = std::pair<std::int32_t, std::int64_t>;

// Independent book reconstruction from the same stream and exec records.
struct Reco {
  struct RO {
    std::int32_t price;
    std::int64_t qty;
    gavel::Side side;
  };
  std::unordered_map<std::uint64_t, RO> orders;
  std::map<std::int32_t, std::int64_t> levels[2];

  static std::size_t idx(gavel::Side s) { return s == gavel::Side::buy ? 0u : 1u; }

  void add(std::uint64_t id, gavel::Side s, std::int32_t px, std::int64_t q) {
    orders.emplace(id, RO{px, q, s});
    levels[idx(s)][px] += q;
  }
  void dec_level(gavel::Side s, std::int32_t px, std::int64_t q) {
    auto& lv = levels[idx(s)];
    auto it = lv.find(px);
    if (it == lv.end()) return;
    it->second -= q;
    if (it->second <= 0) lv.erase(it);
  }
  void reduce(std::uint64_t id, std::int64_t q) {
    auto it = orders.find(id);
    if (it == orders.end()) return;
    const std::int64_t take = q < it->second.qty ? q : it->second.qty;
    dec_level(it->second.side, it->second.price, take);
    it->second.qty -= take;
    if (it->second.qty <= 0) orders.erase(it);
  }
  void cancel(std::uint64_t id) {
    auto it = orders.find(id);
    if (it == orders.end()) return;
    dec_level(it->second.side, it->second.price, it->second.qty);
    orders.erase(it);
  }
  bool replace(std::uint64_t old_id, std::uint64_t new_id, std::int64_t q, std::int32_t px) {
    auto it = orders.find(old_id);
    if (it == orders.end()) return false;
    const gavel::Side s = it->second.side;
    cancel(old_id);
    add(new_id, s, px, q);
    return true;
  }
  std::vector<Lvl> top(gavel::Side s, std::size_t n) const {
    std::vector<Lvl> v;
    const auto& lv = levels[idx(s)];
    if (s == gavel::Side::buy) {
      for (auto it = lv.rbegin(); it != lv.rend() && v.size() < n; ++it) v.emplace_back(it->first, it->second);
    } else {
      for (auto it = lv.begin(); it != lv.end() && v.size() < n; ++it) v.emplace_back(it->first, it->second);
    }
    return v;
  }
};

std::vector<Lvl> engine_top(const gavel::LadderSide& s, std::size_t n) {
  std::vector<Lvl> v;
  for (gavel::Price p = s.best(); p.valid() && v.size() < n; p = s.next_worse(p)) {
    if (const gavel::Level* l = s.level_if(p)) v.emplace_back(p.v, l->agg_qty);
  }
  return v;
}

struct Events {
  std::vector<gavel::EvExecuted> execs;
  std::vector<gavel::EvRejected> rejects;
};

// Walks the emitter wire format: 1 byte type, 1 byte size, payload struct.
void scan(const std::vector<std::uint8_t>& buf, Events& ev) {
  ev.execs.clear();
  ev.rejects.clear();
  std::size_t i = 0;
  while (i + 2 <= buf.size()) {
    const auto t = static_cast<gavel::EventType>(buf[i]);
    const std::size_t sz = buf[i + 1];
    if (i + 2 + sz > buf.size()) break;
    if (t == gavel::EventType::executed && sz == sizeof(gavel::EvExecuted)) {
      gavel::EvExecuted e;
      std::memcpy(&e, buf.data() + i + 2, sizeof(e));
      ev.execs.push_back(e);
    } else if (t == gavel::EventType::rejected && sz == sizeof(gavel::EvRejected)) {
      gavel::EvRejected e;
      std::memcpy(&e, buf.data() + i + 2, sizeof(e));
      ev.rejects.push_back(e);
    }
    i += 2 + sz;
  }
}

int usage() {
  std::fprintf(stderr,
               "usage: gavel-fidelity --stream DIR/SYM.gvl --exec DIR/SYM.exec"
               " [--checkpoint-every N] [--window HALFTICKS]\n");
  return 2;
}

unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

double rate(std::uint64_t n, std::uint64_t d) { return d ? static_cast<double>(n) / static_cast<double>(d) : 0.0; }

}  // namespace

int main(int argc, char** argv) {
  std::string stream_path, exec_path;
  std::uint64_t every = 1000000;
  std::uint32_t window = 1u << 16;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--stream") == 0 && i + 1 < argc) stream_path = argv[++i];
    else if (std::strcmp(argv[i], "--exec") == 0 && i + 1 < argc) exec_path = argv[++i];
    else if (std::strcmp(argv[i], "--checkpoint-every") == 0 && i + 1 < argc) every = std::strtoull(argv[++i], nullptr, 10);
    else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) window = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else return usage();
  }
  if (stream_path.empty() || exec_path.empty()) return usage();

  gavel::StreamReader sr(stream_path);
  if (!sr.ok()) { std::fprintf(stderr, "cannot open stream %s\n", stream_path.c_str()); return 2; }
  gavel::itch::ExecReader er(exec_path);
  if (!er.ok()) { std::fprintf(stderr, "cannot open exec sidecar %s\n", exec_path.c_str()); return 2; }

  gavel::Config cfg;
  cfg.num_symbols = 1;
  cfg.ladder_window = window;
  cfg.checkpoint_interval = 0;
  gavel::Engine eng(cfg);
  Reco reco;
  Events ev;

  std::uint64_t nstream = 0, n_e = 0, n_c = 0;
  std::uint64_t viol_price = 0, viol_time = 0, viol_counter = 0, engine_matches = 0;
  std::uint64_t excl_gone = 0, excl_unknown_target = 0, excl_other_reject = 0, reco_misses = 0;
  std::uint64_t ncheck = 0, nmismatch = 0;
  std::uint64_t last_shift = 0, inj = 0;

  auto inj_seq = [&]() { return last_shift + (++inj); };

  auto feed = [&](const gavel::InputMsg& m) {
    gavel::InputMsg f = m;
    f.seq = m.seq << kShift;
    if (m.type == gavel::MsgType::cancel || m.type == gavel::MsgType::reduce || m.type == gavel::MsgType::replace)
      f.target = m.target << kShift;
    eng.emitter().drain();
    eng.on_msg(f);
    scan(eng.emitter().buffer(), ev);
    // A marketable displayed add executes in the engine where the venue reports an A then an E; this is continuous matching on displayed flow, not a fault.
    engine_matches += ev.execs.size();
    bool rejected = false;
    for (const auto& r : ev.rejects) {
      if (r.seq == f.seq) rejected = true;
      if (r.reason == gavel::Reason::unknown_target) ++excl_unknown_target;
      else ++excl_other_reject;
    }
    if (!rejected) {
      switch (f.type) {
        case gavel::MsgType::enter: reco.add(f.seq, f.side, f.price, f.qty); break;
        case gavel::MsgType::reduce: reco.reduce(f.target, f.qty); break;
        case gavel::MsgType::cancel: reco.cancel(f.target); break;
        case gavel::MsgType::replace:
          if (!reco.replace(f.target, f.seq, f.qty, f.price)) ++reco_misses;
          break;
        default: break;
      }
    }
    last_shift = f.seq;
    inj = 0;
  };

  auto handle_exec = [&](const gavel::itch::ExecRecord& e) {
    const gavel::Ts ets = e.ts & ~std::uint64_t{1};
    const gavel::OrderId id = e.gavel_id << kShift;
    if (e.kind != gavel::itch::kExecKindE) {
      // C prints are not validated; apply them as reductions so the book keeps tracking the venue.
      ++n_c;
      if (eng.find_order(id)) {
        eng.emitter().drain();
        eng.on_msg(gavel::make_reduce(inj_seq(), ets, id, e.qty));
      }
      reco.reduce(id, e.qty);
      return;
    }
    ++n_e;
    const gavel::Order* o = eng.find_order(id);
    if (!o || gavel::ostate(*o) != gavel::OrderState::resting) {
      ++excl_gone;
      reco.reduce(id, e.qty);
      return;
    }
    const gavel::Side side = o->side;
    const gavel::Price px = o->price;
    const gavel::Price best = eng.book(0).side(side).best();
    const bool price_ok = best.valid() && px == best;
    bool time_ok = false;
    int depth = -1;
    if (price_ok) {
      gavel::OrderId front = 0;
      bool first = true;
      int pos = 0, found = -1;
      eng.for_each_order_at(0, side, best, [&](const gavel::Order& fo) {
        if (first) { front = fo.id; first = false; }
        if (fo.id == id) found = pos;
        ++pos;
      });
      time_ok = front == id;
      depth = found;
    }
    if (price_ok && time_ok) {
      // Counterfactual aggressor: an IOC at the resting price must hit exactly this order.
      eng.emitter().drain();
      eng.on_msg(gavel::make_enter(inj_seq(), ets, 0, gavel::opposite(side), gavel::OrderKind::limit,
                                   gavel::Tif::ioc, e.qty, px.v));
      scan(eng.emitter().buffer(), ev);
      const bool ok = ev.execs.size() == 1 && ev.execs[0].resting == id && ev.execs[0].qty == e.qty;
      if (!ok) ++viol_counter;
    } else {
      if (!price_ok) ++viol_price; else ++viol_time;
      if (std::getenv("GAVEL_FID_DIAG") && (viol_price + viol_time) <= 20)
        std::fprintf(stderr, "  diag: price_ok=%d depth=%d best=%d px=%d qty=%d\n",
                     price_ok ? 1 : 0, depth, best.v, px.v, e.qty);
      // Sync the venue fill without matching so the book keeps tracking the tape.
      eng.emitter().drain();
      eng.on_msg(gavel::make_reduce(inj_seq(), ets, id, e.qty));
    }
    reco.reduce(id, e.qty);
  };

  auto compare_books = [&]() {
    ++ncheck;
    const bool same = engine_top(eng.book(0).bid, 10) == reco.top(gavel::Side::buy, 10) &&
                      engine_top(eng.book(0).ask, 10) == reco.top(gavel::Side::sell, 10);
    if (!same) ++nmismatch;
  };

  gavel::itch::ExecRecord ex;
  bool have_ex = er.read(ex);
  gavel::InputMsg m;
  while (sr.read(m)) {
    while (have_ex && ex.after_seq < m.seq) {
      handle_exec(ex);
      have_ex = er.read(ex);
    }
    feed(m);
    ++nstream;
    if (every && nstream % every == 0) compare_books();
  }
  while (have_ex) {
    handle_exec(ex);
    have_ex = er.read(ex);
  }
  compare_books();

  // Hard guarantees scoped to displayed liquidity (see docs/design.md); time priority and book agreement are confounded, so reported as rates, not gated.
  const std::uint64_t hard_violations = viol_price + viol_counter;
  std::printf("stream records                 %llu\n", ull(nstream));
  std::printf("venue executions E             %llu\n", ull(n_e));
  std::printf("venue executions C (excluded)  %llu\n", ull(n_c));
  std::printf("hard guarantees (displayed)\n");
  std::printf("  price priority violations    %llu (%.6f per E)\n", ull(viol_price), rate(viol_price, n_e));
  std::printf("  counterfactual disagreements %llu (%.6f per E)\n", ull(viol_counter), rate(viol_counter, n_e));
  std::printf("reported (confounded by hidden interest, see docs)\n");
  std::printf("  time priority divergence     %llu (%.6f per E)\n", ull(viol_time), rate(viol_time, n_e));
  std::printf("  book agreement               %llu of %llu checkpoints (%.4f)\n",
              ull(ncheck - nmismatch), ull(ncheck), 1.0 - rate(nmismatch, ncheck));
  std::printf("informational\n");
  std::printf("  engine matches on adds       %llu\n", ull(engine_matches));
  std::printf("  exec on missing order        %llu\n", ull(excl_gone));
  std::printf("  unknown target rejects       %llu\n", ull(excl_unknown_target));
  std::printf("  other rejects                %llu\n", ull(excl_other_reject));
  std::printf("  reconstructor replace misses %llu\n", ull(reco_misses));
  std::printf("result                         %s\n", hard_violations == 0 ? "PASS" : "FAIL");
  return hard_violations == 0 ? 0 : 1;
}
