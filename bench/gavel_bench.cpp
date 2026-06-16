// gavel-bench: throughput and latency benchmarks for the gavel engine.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <pthread/qos.h>
#include <sys/sysctl.h>

#include "gavel/engine.hpp"
#include "histogram.hpp"
#include "pmu.hpp"
#include "timer.hpp"
#include "workload.hpp"

#define GB_STR2(x) #x
#define GB_STR(x) GB_STR2(x)

#ifndef GAVEL_BENCH_FLAGS
#define GAVEL_BENCH_FLAGS "unknown"
#endif

namespace {

constexpr std::size_t kDrainBatch = 65536;
constexpr std::size_t kDrainBytes = 1u << 22;
const double kPercentiles[5] = {50.0, 90.0, 99.0, 99.9, 99.99};

using ull = unsigned long long;

std::string sysctl_str(const char* name) {
  char buf[256];
  std::size_t len = sizeof buf;
  if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "unknown";
  return std::string(buf, len > 0 ? len - 1 : 0);
}

std::uint64_t sysctl_u64(const char* name) {
  std::uint64_t v = 0;
  std::size_t len = sizeof v;
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
  return v;
}

struct HostInfo {
  std::string model = sysctl_str("hw.model");
  std::string cpu = sysctl_str("machdep.cpu.brand_string");
  std::uint64_t memsize = sysctl_u64("hw.memsize");
  std::uint64_t pcores = sysctl_u64("hw.perflevel0.physicalcpu");
  std::uint64_t ecores = sysctl_u64("hw.perflevel1.physicalcpu");
};

const char* libcpp_mode() {
#ifdef _LIBCPP_HARDENING_MODE
  return GB_STR(_LIBCPP_HARDENING_MODE);
#else
  return "not defined";
#endif
}

std::string iso_now() {
  char buf[40];
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &tm);
  return buf;
}

class Json {
 public:
  Json& s(const char* k, const std::string& v) {
    key(k);
    buf_ += '"';
    for (char c : v) {
      if (c == '"' || c == '\\') buf_ += '\\';
      buf_ += c;
    }
    buf_ += '"';
    return *this;
  }
  Json& u(const char* k, std::uint64_t v) {
    key(k);
    buf_ += std::to_string(v);
    return *this;
  }
  Json& d(const char* k, double v) {
    char t[40];
    std::snprintf(t, sizeof t, "%.6g", v);
    key(k);
    buf_ += t;
    return *this;
  }
  Json& b(const char* k, bool v) {
    key(k);
    buf_ += v ? "true" : "false";
    return *this;
  }
  Json& raw(const char* k, const std::string& v) {
    key(k);
    buf_ += v;
    return *this;
  }
  std::string str() const { return "{" + buf_ + "}"; }

 private:
  void key(const char* k) {
    if (!buf_.empty()) buf_ += ',';
    buf_ += '"';
    buf_ += k;
    buf_ += "\":";
  }
  std::string buf_;
};

struct Reporter {
  std::FILE* jf{nullptr};
  HostInfo host;
  bool pmu_ok{false};
  double res_ns{0.0};

  ~Reporter() {
    if (jf) std::fclose(jf);
  }
  Json base() const {
    Json j;
    j.s("timestamp", iso_now());
    j.s("hw_model", host.model).s("cpu", host.cpu).u("memsize", host.memsize);
    j.u("p_cores", host.pcores).u("e_cores", host.ecores);
    j.s("gavel_hardening", GB_STR(GAVEL_HARDENING)).s("libcpp_hardening", libcpp_mode());
    j.s("compiler", __VERSION__).s("flags", GAVEL_BENCH_FLAGS);
    j.d("timer_resolution_ns", res_ns).b("pmu_available", pmu_ok);
    return j;
  }
  void write(const Json& j) {
    if (!jf) return;
    std::fputs(j.str().c_str(), jf);
    std::fputc('\n', jf);
    std::fflush(jf);
  }
};

void print_header(const Reporter& r) {
  std::printf("gavel-bench\n");
  std::printf("  build: GAVEL_HARDENING=%s  _LIBCPP_HARDENING_MODE=%s\n",
              GB_STR(GAVEL_HARDENING), libcpp_mode());
  std::printf("  compiler: %s\n", __VERSION__);
  std::printf("  flags: %s\n", GAVEL_BENCH_FLAGS);
  std::printf("  host: %s, %s, %.0f GiB, %llu P cores, %llu E cores\n", r.host.model.c_str(),
              r.host.cpu.c_str(), static_cast<double>(r.host.memsize) / (1024.0 * 1024.0 * 1024.0),
              static_cast<ull>(r.host.pcores), static_cast<ull>(r.host.ecores));
  std::printf("  timer: cntvct_el0 at %llu Hz, %.1f ns per tick; single shot readings below that are quantized\n",
              static_cast<ull>(gbench::tick_freq()), r.res_ns);
  std::printf("  pmu: %s\n", r.pmu_ok
                                 ? "kperf fixed counters active (cycles, instructions)"
                                 : "unavailable (kperf needs root); timer only");
  std::printf("  note: macOS has no core pinning or frequency control; QoS is user_interactive but P vs E placement is decided by the OS\n");
  std::printf("\n");
}

gavel::Config bench_config(std::size_t nevents) {
  gavel::Config cfg;
  // Presize the order pool so growth never lands inside a measurement.
  std::uint32_t cap = 1u << 16;
  while (cap < nevents && cap < (1u << 26)) cap <<= 1;
  cfg.initial_order_capacity = cap;
  return cfg;
}

// One full pass through the workload on a fresh engine; returns events per second.
double one_pass_eps(const gbench::Workload& w) {
  gavel::Engine eng(bench_config(w.events.size()));
  eng.emitter().reserve(kDrainBytes + (1u << 16));
  std::size_t i = 0;
  const std::uint64_t t0 = gbench::now_ticks();
  for (const auto& m : w.events) {
    eng.on_msg(m);
    if ((++i % kDrainBatch) == 0) eng.emitter().drain();
  }
  const std::uint64_t t1 = gbench::now_ticks();
  const double sec = static_cast<double>(gbench::ticks_to_ns(t1 - t0)) * 1e-9;
  return static_cast<double>(w.events.size()) / sec;
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void print_ns(std::uint64_t v) { std::printf(" %8llu%c", static_cast<ull>(v), v < 100 ? '*' : ' '); }

void print_hist_row(const char* mode, const char* rate, const gbench::Histogram& h) {
  std::printf("  %-30s %-16s", mode, rate);
  for (double p : kPercentiles) print_ns(h.percentile(p));
  print_ns(h.max());
  std::printf(" %10llu\n", static_cast<ull>(h.count()));
}

void print_hist_header() {
  std::printf("  %-30s %-16s %8s  %8s  %8s  %8s  %8s  %8s  %10s\n", "mode", "rate", "p50", "p90",
              "p99", "p99.9", "p99.99", "max", "n");
}

void print_footnote() {
  std::printf("  * below 100 ns: at or near the 41.7 ns timer tick, value is quantized; see the batched row for sub resolution means\n");
}

Json& add_hist(Json& j, const gbench::Histogram& h) {
  j.u("count", h.count());
  j.u("p50_ns", h.percentile(50.0)).u("p90_ns", h.percentile(90.0)).u("p99_ns", h.percentile(99.0));
  j.u("p999_ns", h.percentile(99.9)).u("p9999_ns", h.percentile(99.99)).u("max_ns", h.max());
  return j;
}

double run_throughput(Reporter& rep, const gbench::Workload& w, int reps, bool print) {
  std::vector<double> rates;
  rates.reserve(static_cast<std::size_t>(reps));
  if (print)
    std::printf("== throughput: %s (%llu events, %d reps) ==\n", w.name.c_str(),
                static_cast<ull>(w.events.size()), reps);
  for (int r = 0; r < reps; ++r) {
    rates.push_back(one_pass_eps(w));
    if (print) std::printf("  rep %2d: %7.3f M events/s\n", r + 1, rates.back() * 1e-6);
  }
  const double med = median_of(rates);
  const double lo = *std::min_element(rates.begin(), rates.end());
  const double hi = *std::max_element(rates.begin(), rates.end());
  if (print) {
    std::printf("  median %.3f M/s  min %.3f  max %.3f  spread %.1f%% of median\n\n", med * 1e-6,
                lo * 1e-6, hi * 1e-6, (hi - lo) / med * 100.0);
    std::string arr = "[";
    for (std::size_t i = 0; i < rates.size(); ++i) {
      char t[32];
      std::snprintf(t, sizeof t, "%s%.6g", i ? "," : "", rates[i]);
      arr += t;
    }
    arr += "]";
    Json j = rep.base();
    j.s("workload", w.name).s("mode", "throughput").u("events", w.events.size());
    j.u("reps", static_cast<std::uint64_t>(reps)).raw("rates_eps", arr);
    j.d("median_eps", med).d("min_eps", lo).d("max_eps", hi);
    rep.write(j);
  }
  return med;
}

void run_latency(Reporter& rep, const gbench::Workload& w) {
  const std::size_t n = w.events.size();
  std::printf("== latency: %s (%llu events) ==\n", w.name.c_str(), static_cast<ull>(n));

  // Closed loop: timestamp around each on_msg call.
  gbench::Histogram closed;
  {
    gavel::Engine eng(bench_config(n));
    eng.emitter().reserve(kDrainBytes + (1u << 16));
    for (const auto& m : w.events) {
      const std::uint64_t t0 = gbench::now_ticks();
      eng.on_msg(m);
      const std::uint64_t t1 = gbench::now_ticks();
      closed.record(gbench::ticks_to_ns(t1 - t0));
      if (eng.emitter().buffer().size() > kDrainBytes) eng.emitter().drain();
    }
  }

  // Batched closed loop: mean over 1024 event batches, for medians below timer resolution.
  gbench::Histogram batched;
  {
    constexpr std::size_t kBatch = 1024;
    gavel::Engine eng(bench_config(n));
    eng.emitter().reserve(kDrainBytes + (1u << 16));
    for (std::size_t i = 0; i < n; i += kBatch) {
      const std::size_t end = std::min(i + kBatch, n);
      const std::uint64_t t0 = gbench::now_ticks();
      for (std::size_t k = i; k < end; ++k) eng.on_msg(w.events[k]);
      const std::uint64_t t1 = gbench::now_ticks();
      const double mean =
          static_cast<double>(gbench::ticks_to_ns(t1 - t0)) / static_cast<double>(end - i);
      batched.record(static_cast<std::uint64_t>(std::llround(mean)));
      if (eng.emitter().buffer().size() > kDrainBytes) eng.emitter().drain();
    }
  }

  // PMU pass: fixed counters over an untimed full run, when running as root.
  auto& pmu = gbench::Pmu::instance();
  double cyc_per_ev = 0.0, ins_per_ev = 0.0;
  if (pmu.available()) {
    gavel::Engine eng(bench_config(n));
    eng.emitter().reserve(kDrainBytes + (1u << 16));
    std::size_t i = 0;
    pmu.begin();
    for (const auto& m : w.events) {
      eng.on_msg(m);
      if ((++i % kDrainBatch) == 0) eng.emitter().drain();
    }
    const gbench::PmuCounts c = pmu.end();
    cyc_per_ev = static_cast<double>(c.cycles) / static_cast<double>(n);
    ins_per_ev = static_cast<double>(c.instructions) / static_cast<double>(n);
  }

  // Open loop: scheduled arrivals at fractions of measured max throughput.
  const double max_eps = run_throughput(rep, w, 3, false);
  const double fracs[3] = {0.50, 0.80, 0.95};
  gbench::Histogram open[3];
  double achieved[3] = {0.0, 0.0, 0.0};
  for (int f = 0; f < 3; ++f) {
    const double rate = fracs[f] * max_eps;
    const double ticks_per_ev = static_cast<double>(gbench::tick_freq()) / rate;
    gavel::Engine eng(bench_config(n));
    eng.emitter().reserve(kDrainBytes + (1u << 16));
    const std::uint64_t t0 = gbench::now_ticks();
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t sched =
          t0 + static_cast<std::uint64_t>(static_cast<double>(i) * ticks_per_ev);
      while (gbench::now_ticks() < sched) {}
      eng.on_msg(w.events[i]);
      const std::uint64_t done = gbench::now_ticks();
      open[f].record(gbench::ticks_to_ns(done - sched));
      if (eng.emitter().buffer().size() > kDrainBytes) eng.emitter().drain();
    }
    const std::uint64_t t_end = gbench::now_ticks();
    achieved[f] = static_cast<double>(n) /
                  (static_cast<double>(gbench::ticks_to_ns(t_end - t0)) * 1e-9);
  }

  print_hist_header();
  print_hist_row("closed loop (per event)", "unthrottled", closed);
  print_hist_row("closed loop batched x1024", "unthrottled", batched);
  char rate_label[3][32];
  for (int f = 0; f < 3; ++f) {
    std::snprintf(rate_label[f], sizeof rate_label[f], "%.2fM/s (%.0f%%)", fracs[f] * max_eps * 1e-6,
                  fracs[f] * 100.0);
    char mode[40];
    std::snprintf(mode, sizeof mode, "open loop %.0f%% of max", fracs[f] * 100.0);
    print_hist_row(mode, rate_label[f], open[f]);
  }
  print_footnote();
  std::printf("  closed loop excludes queueing delay; open loop latency = completion minus scheduled arrival (coordinated omission safe)\n");
  std::printf("  batched row holds per batch means (1024 events per sample); percentiles are over batch means, not single events\n");
  for (int f = 0; f < 3; ++f)
    std::printf("  open loop %.0f%%: target %.2f M/s, achieved %.2f M/s%s\n", fracs[f] * 100.0,
                fracs[f] * max_eps * 1e-6, achieved[f] * 1e-6,
                achieved[f] < 0.98 * fracs[f] * max_eps ? " (saturated: arrivals outpaced the harness, latencies include queueing)" : "");
  if (pmu.available())
    std::printf("  pmu: %.1f cycles/event, %.1f instructions/event (fixed counters, whole run)\n",
                cyc_per_ev, ins_per_ev);
  else
    std::printf("  pmu: unavailable (kperf needs root), cycles per event not measured\n");
  std::printf("\n");

  Json jc = rep.base();
  jc.s("workload", w.name).s("mode", "latency_closed_loop").d("target_rate_eps", 0.0);
  add_hist(jc, closed);
  if (pmu.available()) jc.d("cycles_per_event", cyc_per_ev).d("instructions_per_event", ins_per_ev);
  rep.write(jc);

  Json jb = rep.base();
  jb.s("workload", w.name).s("mode", "latency_closed_loop_batched").u("batch_size", 1024);
  add_hist(jb, batched);
  rep.write(jb);

  for (int f = 0; f < 3; ++f) {
    Json j = rep.base();
    j.s("workload", w.name).s("mode", "latency_open_loop");
    j.d("target_rate_eps", fracs[f] * max_eps).d("rate_fraction_of_max", fracs[f]);
    j.d("measured_max_eps", max_eps).d("achieved_rate_eps", achieved[f]);
    add_hist(j, open[f]);
    rep.write(j);
  }
}

int usage() {
  std::fprintf(stderr,
               "usage: gavel-bench <throughput|latency|hardening> [options]\n"
               "  --workload NAME   add_cancel | sweep_heavy | mixed_realistic (default: all three)\n"
               "  --replay PATH     use a recorded .gvl input stream as the only workload\n"
               "  --events N        synthetic events per workload (default: throughput 5000000, latency 1000000)\n"
               "  --reps N          throughput repetitions (default 10)\n"
               "  --seed S          workload PRNG seed (default 1)\n"
               "  --json PATH       append one JSON object per measurement to PATH\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  // Hint, not a guarantee: the OS still decides P vs E core placement.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  if (cmd != "throughput" && cmd != "latency" && cmd != "hardening") return usage();

  std::string only, replay, json_path;
  std::size_t events = 0;
  int reps = 10;
  std::uint64_t seed = 1;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_val = i + 1 < argc;
    if (a == "--workload" && has_val) only = argv[++i];
    else if (a == "--replay" && has_val) replay = argv[++i];
    else if (a == "--events" && has_val) events = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    else if (a == "--reps" && has_val) reps = std::atoi(argv[++i]);
    else if (a == "--seed" && has_val) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--json" && has_val) json_path = argv[++i];
    else return usage();
  }
  if (events == 0) events = cmd == "throughput" ? 5'000'000 : 1'000'000;
  if (reps < 1) reps = 1;

  Reporter rep;
  rep.res_ns = gbench::tick_resolution_ns();
  rep.pmu_ok = gbench::Pmu::instance().available();
  if (!json_path.empty()) {
    rep.jf = std::fopen(json_path.c_str(), "a");
    if (!rep.jf) {
      std::fprintf(stderr, "cannot open %s for writing\n", json_path.c_str());
      return 1;
    }
  }
  print_header(rep);
  if (cmd == "hardening") {
    std::printf("hardening matrix runs are driven externally: rebuild with -DGAVEL_HARDENING=0/1\n");
    std::printf("and -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST or _EXTENSIVE on gavel_core,\n");
    std::printf("then rerun throughput and latency; the header above makes each result self describing.\n");
    return 0;
  }

  std::vector<gbench::Workload> work;
  if (!replay.empty()) {
    gbench::Workload w;
    if (!gbench::load_stream(replay, w)) {
      std::fprintf(stderr, "cannot read stream %s\n", replay.c_str());
      return 1;
    }
    work.push_back(std::move(w));
  } else {
    if (only.empty() || only == "add_cancel") work.push_back(gbench::make_add_cancel(seed, events));
    if (only.empty() || only == "sweep_heavy") work.push_back(gbench::make_sweep_heavy(seed, events));
    if (only.empty() || only == "mixed_realistic") work.push_back(gbench::make_mixed_realistic(seed, events));
    if (work.empty()) {
      std::fprintf(stderr, "unknown workload %s\n", only.c_str());
      return usage();
    }
  }

  for (const auto& w : work) {
    if (cmd == "throughput") run_throughput(rep, w, reps, true);
    else run_latency(rep, w);
  }
  return 0;
}
