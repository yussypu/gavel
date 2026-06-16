# gavel

A deterministic exchange matching engine in C++23, verification first.

The design invariant is that the sequenced input stream fully determines every
output byte. Same stream, same binary, same output hash, every run, and the tests
prove it including across optimization levels. The main components are the
determinism proof, the deterministic simulation testing harness, the fidelity check
against a real NASDAQ trading day, and the contracts and hardened mode cost study.

## What it is

- A single threaded matching engine in the Island/INET lineage: a sequencer in
  front, OUCH style order entry semantics in, an ITCH style event stream out.
- Price time priority and these order types: market, limit, IOC, FOK, iceberg,
  primary and market and midpoint pegs, stop and stop limit, self match prevention,
  and opening and closing auctions with a published crossing algorithm.
- No floating point in core, no clock reads, no I/O, no allocation on the event
  path, no third party dependencies.

## Results

Measured on an Apple M1 laptop, Apple clang 17, Release with `-Werror`. Numbers are
engine service time, not door to door, and the benchmark output is self describing
about its platform limits.

- Determinism: bit identical output hashes across O0, O2, and O3, across all six DST
  presets, and across repeated runs (which rules out address space dependence).
- Tests: seven suites green, including a 47 case semantics suite with 1882 assertions
  and an id map fuzz against `std::unordered_map`.
- Soak: 34 deterministic simulation runs, zero failures, about 194 million events,
  with a tier that checks invariants and an independent shadow book after every event.
- Fidelity: a full NASDAQ trading day for six symbols, about 91,700 venue executions,
  zero displayed price priority violations and zero counterfactual aggressor
  disagreements (the two guarantees public ITCH permits).
- Throughput: 9.58 M events/s on add heavy flow, 6.41 M on a realistic mix, 7.05 M on
  real ITCH derived AAPL flow.
- Latency per event on add heavy flow: p50 around 83 ns (at the 41.7 ns timer tick,
  so quantized; batched mean 105 ns), p99 334 ns, p99.9 458 ns.

Full distributions, methodology, and what would change on Linux x86_64 are in
`docs/benchmark-report.md`.

## Documentation

- `docs/semantics.md` the normative contract: code implements it, tests test it.
- `docs/design.md` the design rationale and tradeoffs.
- `docs/architecture.md` the as-built structure.
- `docs/technical-deep-dive.md` the subtle internals: book, pegs, stops, auction cross.
- `docs/benchmark-report.md` the measurements and method.
- `docs/lessons-learned.md` what the verification harness taught.

## Build

Requires CMake, Ninja, and a C++23 compiler (developed with Apple clang 17 on an
Apple M1).

```
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

Seven test suites: a smoke test, the 47 case engine semantics suite, an id map fuzz
against std::unordered_map, an SPSC ring stress test, the DST driver, the ITCH
parser and fidelity asserts, and the histogram against a reference.

## Reproduce the determinism guarantee

Build at three optimization levels and confirm the output hash is bit identical on
the same input stream (run `scripts/get_data.sh` first to fetch the AAPL stream, or
point `gavel-replay` at any input):

```
for opt in O0 O2 O3; do
  cmake -G Ninja -S . -B build-$opt -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-$opt -DNDEBUG" >/dev/null
  ninja -C build-$opt gavel-replay >/dev/null
  build-$opt/tools/gavel-replay --symbols 1 data/20200130/AAPL.gvl | grep "output hash"
done
```

## Deterministic simulation testing

```
build/tools/gavel-sim --seed 1 --events 1000000 --preset peg_heavy --check-every 1000
scripts/soak.sh soak.log    # the bounded multi tier soak
```

The driver runs the engine with invariant checks and an independent shadow book; on
a violation, rerun with `--shrink` to delta debug the input down to a minimal
reproducer written to `repros/`.

## Fidelity against real NASDAQ data

The extracted ITCH streams are not committed (they are derived NASDAQ data and large).
`scripts/get_data.sh` regenerates them by streaming a TotalView-ITCH sample day through
`gavel-extract`, nothing large touching disk. The per symbol fidelity reports under
`data/20200130/fidelity/` are committed. To regenerate the streams:

```
build/tools/gavel-fidelity --stream data/20200130/AAPL.gvl --exec data/20200130/AAPL.exec
```

This reports the guarantees public ITCH can verify (displayed price priority,
counterfactual aggressor agreement) separately from the metrics it cannot (order
level time priority, book agreement during marketable add windows). See
`docs/benchmark-report.md` and the per symbol outputs in
`data/20200130/fidelity/`.

## Benchmarks

```
build/bench/gavel-bench throughput --json results/throughput.jsonl
build/bench/gavel-bench latency    --json results/latency.jsonl
```

Full percentile distributions, environment capture, and coordinated omission aware
open loop load generation. The host is Apple Silicon with no core or frequency
pinning and a 41.7 ns timer; the benchmark output is self describing about its
platform limits and the report states what would change on Linux x86_64. See
`docs/benchmark-report.md`.

## Layout

- `include/gavel/` and `src/` the engine core.
- `include/gavel/itch/` and `tools/gavel_extract.cpp`, `tools/gavel_fidelity.cpp` the
  ITCH path.
- `include/gavel/verify/` the DST harness, generator, shadow book, invariants.
- `bench/` the measurement tooling.
- `tools/` the CLI binaries.
- `scripts/` data fetch and soak.
- `.github/workflows/ci.yml` CI: macOS arm64 and Linux x86_64, sanitizers, the
  hardening matrix, and the O0 versus O3 determinism check.

## License

MIT, see `LICENSE`. The vendored `third_party/doctest/doctest.h` keeps its own MIT
license header.
