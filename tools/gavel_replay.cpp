#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include "gavel/engine.hpp"
#include "gavel/stream.hpp"

// Replays a gavel input stream file and reports output hash and throughput.
int main(int argc, char** argv) {
  std::uint32_t symbols = 1;
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) symbols = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    else if (argv[i][0] != '-') path = argv[i];
    else { std::fprintf(stderr, "usage: gavel-replay [--symbols N] stream.gvl\n"); return 2; }
  }
  if (!path) { std::fprintf(stderr, "usage: gavel-replay [--symbols N] stream.gvl\n"); return 2; }
  gavel::StreamReader in(path);
  if (!in.ok()) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
  gavel::Config cfg;
  cfg.num_symbols = symbols;
  gavel::Engine eng(cfg);
  gavel::InputMsg m;
  std::uint64_t n = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (in.read(m)) {
    eng.on_msg(m);
    ++n;
    if (eng.emitter().buffer().size() > (1u << 24)) eng.emitter().drain();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  std::printf("messages       %llu\n", static_cast<unsigned long long>(n));
  std::printf("output events  %llu\n", static_cast<unsigned long long>(eng.emitter().count()));
  std::printf("output hash    %016llx\n", static_cast<unsigned long long>(eng.emitter().hash()));
  std::printf("book digest    %016llx\n", static_cast<unsigned long long>(eng.book_digest()));
  std::printf("elapsed        %.3f s  (%.2f M msg/s)\n", sec, n / sec / 1e6);
  return 0;
}
