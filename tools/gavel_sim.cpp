// DST driver: deterministic flow, invariant and shadow checks, shrinking.
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "gavel/engine.hpp"
#include "gavel/stream.hpp"
#include "gavel/verify/generator.hpp"
#include "gavel/verify/invariants.hpp"
#include "gavel/verify/shadow.hpp"

using namespace gavel;

namespace {

struct Options {
  std::uint64_t seed{1};
  std::uint64_t events{100000};
  std::string preset{"default"};
  std::uint64_t check_every{1};
  std::uint32_t symbols{4};
  bool snapshot_test{false};
  bool shrink{false};
  std::string replay;
  std::string repro_dir{"repros"};
};

void usage() {
  std::fprintf(stderr,
               "usage: gavel-sim --seed S --events N [--preset NAME] [--check-every K]\n"
               "                 [--symbols M] [--snapshot-test] [--shrink]\n"
               "                 [--replay FILE] [--repro-dir DIR]\n"
               "presets: default peg_heavy stop_cascade iceberg_sweep auction_mix tight_book\n");
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--seed") {
      const char* v = val();
      if (!v) return false;
      o.seed = std::strtoull(v, nullptr, 0);
    } else if (a == "--events") {
      const char* v = val();
      if (!v) return false;
      o.events = std::strtoull(v, nullptr, 0);
    } else if (a == "--preset") {
      const char* v = val();
      if (!v) return false;
      o.preset = v;
    } else if (a == "--check-every") {
      const char* v = val();
      if (!v) return false;
      o.check_every = std::strtoull(v, nullptr, 0);
    } else if (a == "--symbols") {
      const char* v = val();
      if (!v) return false;
      o.symbols = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 0));
    } else if (a == "--snapshot-test") {
      o.snapshot_test = true;
    } else if (a == "--shrink") {
      o.shrink = true;
    } else if (a == "--replay") {
      const char* v = val();
      if (!v) return false;
      o.replay = v;
    } else if (a == "--repro-dir") {
      const char* v = val();
      if (!v) return false;
      o.repro_dir = v;
    } else {
      return false;
    }
  }
  return true;
}

Config engine_config(std::uint32_t symbols) {
  Config c;
  c.num_symbols = symbols;
  c.ladder_window = 8192;
  c.initial_order_capacity = 1u << 16;
  c.max_orders = 1u << 26;
  c.checkpoint_interval = 1000;
  return c;
}

struct Outcome {
  std::vector<std::string> violations;
  std::size_t fail_index{0};  // msgs consumed when the first violation surfaced
  std::uint64_t hash{0};
  std::uint64_t digest{0};
  std::size_t max_live{0};
};

Outcome run_stream(const std::vector<InputMsg>& msgs, const Config& cfg,
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
    if (!out.violations.empty() && out.fail_index == 0) out.fail_index = i;
    return !out.violations.empty();
  };
  for (const InputMsg& m : msgs) {
    eng.on_msg(m);
    ++i;
    if (check_every) {
      if (i % check_every == 0 && inspect()) break;
    } else if (eng.emitter().buffer().size() > (1u << 22)) {
      eng.emitter().drain();
    }
  }
  if (check_every && out.violations.empty()) inspect();
  out.hash = eng.emitter().hash();
  out.digest = eng.book_digest();
  out.max_live = shadow.max_live();
  return out;
}

const char* kind_name(OrderKind k) {
  switch (k) {
    case OrderKind::limit: return "limit";
    case OrderKind::market: return "market";
    case OrderKind::peg_primary: return "peg_primary";
    case OrderKind::peg_market: return "peg_market";
    case OrderKind::peg_mid: return "peg_mid";
    case OrderKind::stop: return "stop";
    case OrderKind::stop_limit: return "stop_limit";
  }
  return "?";
}

const char* tif_name(Tif t) {
  switch (t) {
    case Tif::day: return "day";
    case Tif::ioc: return "ioc";
    case Tif::fok: return "fok";
  }
  return "?";
}

const char* clock_name(std::uint8_t a) {
  switch (static_cast<ClockAction>(a)) {
    case ClockAction::session_start: return "session_start";
    case ClockAction::open_cross: return "open_cross";
    case ClockAction::close_cross: return "close_cross";
    case ClockAction::session_end: return "session_end";
  }
  return "?";
}

void print_msg(const InputMsg& m) {
  const auto seq = static_cast<unsigned long long>(m.seq);
  const auto ts = static_cast<unsigned long long>(m.ts);
  const auto tgt = static_cast<unsigned long long>(m.target);
  switch (m.type) {
    case MsgType::enter:
      std::printf("  seq=%llu ts=%llu enter sym=%u %s %s %s qty=%d price=%d aux=%d part=%u smp=%u\n",
                  seq, ts, m.symbol, m.side == Side::buy ? "buy" : "sell", kind_name(m.kind),
                  tif_name(m.tif), m.qty, m.price, m.aux, m.participant,
                  static_cast<unsigned>(m.flags & 3));
      return;
    case MsgType::cancel:
      std::printf("  seq=%llu ts=%llu cancel target=%llu\n", seq, ts, tgt);
      return;
    case MsgType::reduce:
      std::printf("  seq=%llu ts=%llu reduce target=%llu by=%d\n", seq, ts, tgt, m.qty);
      return;
    case MsgType::replace:
      std::printf("  seq=%llu ts=%llu replace target=%llu qty=%d price=%d\n", seq, ts, tgt, m.qty,
                  m.price);
      return;
    case MsgType::clock:
      std::printf("  seq=%llu ts=%llu clock %s\n", seq, ts, clock_name(m.clock_action));
      return;
  }
  std::printf("  seq=%llu ts=%llu unknown type %u\n", seq, ts, static_cast<unsigned>(m.type));
}

// Delta debugging: failing prefix, then chunk removal at N/2, N/4, ... 1 keeping original seqs.
std::vector<InputMsg> shrink_failure(std::vector<InputMsg> msgs, const Config& cfg) {
  auto fails = [&](const std::vector<InputMsg>& c) {
    return !run_stream(c, cfg, 1).violations.empty();
  };
  const Outcome o = run_stream(msgs, cfg, 1);
  if (o.violations.empty()) return {};
  msgs.resize(o.fail_index);
  std::size_t budget = 5000;
  for (std::size_t chunk = msgs.size() / 2; chunk >= 1; chunk /= 2) {
    bool progress = true;
    while (progress && budget) {
      progress = false;
      std::size_t start = 0;
      while (start < msgs.size() && budget) {
        const std::size_t end = std::min(start + chunk, msgs.size());
        std::vector<InputMsg> cand;
        cand.reserve(msgs.size() - (end - start));
        cand.insert(cand.end(), msgs.begin(),
                    msgs.begin() + static_cast<std::ptrdiff_t>(start));
        cand.insert(cand.end(), msgs.begin() + static_cast<std::ptrdiff_t>(end), msgs.end());
        --budget;
        if (!cand.empty() && cand.size() < msgs.size() && fails(cand)) {
          msgs.swap(cand);
          progress = true;
        } else {
          start += chunk;
        }
      }
    }
    if (chunk == 1) break;
  }
  return msgs;
}

bool snapshot_self_test(const std::vector<InputMsg>& msgs, const Config& cfg, std::string& err) {
  Engine a(cfg);
  const std::size_t mid = msgs.size() / 2;
  for (std::size_t i = 0; i < mid; ++i) {
    a.on_msg(msgs[i]);
    if (a.emitter().buffer().size() > (1u << 22)) a.emitter().drain();
  }
  std::vector<std::uint8_t> snap;
  a.save_snapshot(snap);
  for (std::size_t i = mid; i < msgs.size(); ++i) {
    a.on_msg(msgs[i]);
    if (a.emitter().buffer().size() > (1u << 22)) a.emitter().drain();
  }
  Engine b(cfg);
  b.load_snapshot(snap.data(), snap.size());
  for (std::size_t i = mid; i < msgs.size(); ++i) {
    b.on_msg(msgs[i]);
    if (b.emitter().buffer().size() > (1u << 22)) b.emitter().drain();
  }
  if (a.emitter().hash() != b.emitter().hash() || a.book_digest() != b.book_digest()) {
    err = "snapshot replay mismatch: full hash " + verify::hex64(a.emitter().hash()) +
          " digest " + verify::hex64(a.book_digest()) + " vs restored hash " +
          verify::hex64(b.emitter().hash()) + " digest " + verify::hex64(b.book_digest());
    return false;
  }
  return true;
}

int report_failure(const Options& opt, const std::vector<InputMsg>& msgs, const Config& cfg,
                   const Outcome& bad) {
  std::printf("FAIL: preset=%s seed=%llu violations at msg %zu\n", opt.preset.c_str(),
              static_cast<unsigned long long>(opt.seed), bad.fail_index);
  for (std::size_t i = 0; i < bad.violations.size() && i < 10; ++i)
    std::printf("  %s\n", bad.violations[i].c_str());
  if (!opt.shrink) {
    std::printf("rerun with --shrink to minimize\n");
    return 1;
  }
  std::printf("shrinking %zu messages...\n", msgs.size());
  const std::vector<InputMsg> minimal = shrink_failure(msgs, cfg);
  if (minimal.empty()) {
    std::printf("shrink could not reproduce the failure deterministically\n");
    return 1;
  }
  ::mkdir(opt.repro_dir.c_str(), 0755);
  const std::string path =
      opt.repro_dir + "/repro_" + opt.preset + "_seed" + std::to_string(opt.seed) + ".stream";
  StreamWriter w(path);
  if (!w.ok()) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  for (const InputMsg& m : minimal) w.write(m);
  w.close();
  const Outcome mo = run_stream(minimal, cfg, 1);
  std::printf("minimal repro: %zu messages -> %s\n", minimal.size(), path.c_str());
  for (const InputMsg& m : minimal) print_msg(m);
  std::printf("violation on minimal stream:\n");
  for (std::size_t i = 0; i < mo.violations.size() && i < 10; ++i)
    std::printf("  %s\n", mo.violations[i].c_str());
  std::printf("reproduce with: gavel-sim --replay %s --symbols %u\n", path.c_str(), opt.symbols);
  return 1;
}

int do_replay(const Options& opt) {
  StreamReader r(opt.replay);
  if (!r.ok()) {
    std::fprintf(stderr, "cannot read %s\n", opt.replay.c_str());
    return 2;
  }
  std::vector<InputMsg> msgs;
  InputMsg m;
  while (r.read(m)) msgs.push_back(m);
  const Config cfg = engine_config(opt.symbols);
  const Outcome o = run_stream(msgs, cfg, 1);
  std::printf("replayed %zu messages\n", msgs.size());
  for (const InputMsg& mm : msgs) print_msg(mm);
  if (o.violations.empty()) {
    std::printf("clean: hash=%s digest=%s\n", verify::hex64(o.hash).c_str(),
                verify::hex64(o.digest).c_str());
    return 0;
  }
  for (std::size_t i = 0; i < o.violations.size() && i < 10; ++i)
    std::printf("  %s\n", o.violations[i].c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    usage();
    return 2;
  }
  if (!opt.replay.empty()) return do_replay(opt);
  if (!sim::known_preset(opt.preset)) {
    std::fprintf(stderr, "unknown preset %s\n", opt.preset.c_str());
    usage();
    return 2;
  }
  sim::GenConfig gc = sim::preset_config(opt.preset);
  gc.num_symbols = opt.symbols;
  const Config cfg = engine_config(opt.symbols);
  const std::vector<InputMsg> msgs = sim::Generator(opt.seed, gc).generate(opt.events);

  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const Outcome checked = run_stream(msgs, cfg, opt.check_every);
  const auto t1 = clock::now();
  if (!checked.violations.empty()) return report_failure(opt, msgs, cfg, checked);

  // Determinism self test: same input twice, checks off the second time.
  const auto t2 = clock::now();
  const Outcome raw = run_stream(msgs, cfg, 0);
  const auto t3 = clock::now();
  if (raw.hash != checked.hash || raw.digest != checked.digest) {
    std::printf("FAIL: determinism: run1 hash=%s digest=%s run2 hash=%s digest=%s\n",
                verify::hex64(checked.hash).c_str(), verify::hex64(checked.digest).c_str(),
                verify::hex64(raw.hash).c_str(), verify::hex64(raw.digest).c_str());
    return 1;
  }

  if (opt.snapshot_test) {
    std::string err;
    if (!snapshot_self_test(msgs, cfg, err)) {
      std::printf("FAIL: %s\n", err.c_str());
      return 1;
    }
  }

  const double sec_checked = std::chrono::duration<double>(t1 - t0).count();
  const double sec_raw = std::chrono::duration<double>(t3 - t2).count();
  const double n = static_cast<double>(msgs.size());
  std::printf(
      "clean: preset=%s seed=%llu msgs=%zu hash=%s digest=%s max_live=%zu "
      "checked=%.0f msgs/s raw=%.0f msgs/s%s\n",
      opt.preset.c_str(), static_cast<unsigned long long>(opt.seed), msgs.size(),
      verify::hex64(checked.hash).c_str(), verify::hex64(checked.digest).c_str(),
      checked.max_live, n / (sec_checked > 0 ? sec_checked : 1e-9),
      n / (sec_raw > 0 ? sec_raw : 1e-9), opt.snapshot_test ? " snapshot=ok" : "");
  return 0;
}
