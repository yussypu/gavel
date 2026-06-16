# gavel design

gavel is a deterministic exchange in modern C++, verification first. This document
records the design rationale and the tradeoffs behind it. The as built structure is
in architecture.md, the internals in technical-deep-dive.md, and the normative
behavior in semantics.md.

## 1. Architecture

The Island/INET architecture, reduced to its provable core: a sequencer establishes a total order over inbound events; a single threaded matching engine consumes the sequenced stream and emits an output event stream; every downstream view (market data, order acks, state snapshots) is a pure function of the sequenced input. Determinism is not a property we hope for, it is the design invariant everything else tests against.

```
 inbound (order entry, clock events)
        |
   [ sequencer ]  assigns seq no + logical timestamp
        |
   sequenced input log  (the single source of truth, replayable)
        |
   [ matching engine ]  single threaded, no clock, no syscalls, no allocation
        |
   output event stream  (acks, executions, book deltas, state hashes)
```

Components:

- core: types, sequencer, order book, matching engine, auction cross, output emitter. Freestanding library, no I/O, no clock reads, no third party dependencies.
- itch: NASDAQ TotalView-ITCH 5.0 parser, book reconstructor, and the extractor that converts real ITCH days into gavel input streams.
- verify: invariant checker, property test generators, deterministic simulation harness with delta debugging shrinker, fidelity validator.
- bench: open loop load generator, HDR style histogram, cntvct_el0 timer, kperf PMU counters, environment capture.
- tools: CLI binaries wrapping the above (gavel-replay, gavel-sim, gavel-bench, gavel-extract).

## 2. Core technical design

### 2.1 Numerics and identity

- Prices are int32 in units of half ticks. Displayable prices are even; midpoint executions land on odd values. No floating point exists anywhere in core. Half tick scaling buys midpoint pegs without a second price domain.
- Quantities are int32 shares (NASDAQ max order size fits comfortably). Order ids are uint64 assigned by the sequencer, dense from 1, so they double as slab indices. Client supplied tokens map to engine ids at the boundary.
- Symbols are uint16 indices into a symbol table fixed at session start.

### 2.2 Order book

- Per symbol ladder: contiguous array of price levels indexed by price offset from a session anchor, with a separate intrusive doubly linked FIFO of orders per level. Best bid and ask tracked by cursor walk, which is branch cheap because book updates near the touch dominate real flow.
- Orders live in a slab pool indexed by order id. No allocation after session start; pool growth only between events. Order nodes hold the intrusive links, so cancel by id is O(1): slab lookup, unlink, level aggregate update.
- Price levels far outside the ladder window (more than 64k half ticks from anchor) are rejected as out of band, matching real exchange price collars. This is a documented business rule.
- Hidden and midpoint pegged orders live in small per symbol side lists checked during matching, mirroring how non displayed interest actually behaves: no book delta on entry, execution reports on trade.

### 2.3 Matching semantics

Price time priority, displayed before hidden at the same price. Incoming marketable orders sweep levels in price order, FIFO within level. Order types:

- limit (day), market, IOC, FOK (all or none against the visible plus hidden book, computed before any fill).
- iceberg: display quantity plus reserve; refill re enters the back of the FIFO at the same level (time priority lost on refill, the NASDAQ behavior).
- pegged: primary peg (joins same side best) and market peg (offset from opposite best), integer tick offsets; midpoint peg as hidden order at the odd half tick. Repricing runs from a deterministic reprice queue drained after every book changing event, in (price, time) order, before the next sequenced event.
- stop and stop limit: trigger on last sale price; triggered orders are processed in time priority order after the triggering match completes, before the next sequenced event.
- self match prevention: participant id per order; policies cancel resting, cancel incoming, cancel both. Checked at match time.

Auctions: opening and closing cross. Crossing price maximizes executable volume; ties minimize absolute imbalance; remaining ties pick the price closest to the last sale reference. The algorithm is published in the docs and property tested against a brute force oracle. Auction transitions are driven by clock events that arrive through the sequencer, so even time is part of the deterministic stream.

### 2.4 Determinism rules (enforced, not aspirational)

1. Core never reads a clock, never does I/O, never allocates on the event path, never iterates an unordered container into output, and contains no floats.
2. All tie breaks resolve by sequence number.
3. Every emitted byte goes through one emitter that maintains a running 64 bit hash (FNV 1a) with periodic checkpoint events carrying the hash and a book state digest.
4. The test suite proves: same stream, same output hash across repeated runs, across O0/O2/O3, and across snapshot plus tail replay versus full replay.

### 2.5 Boundaries

One SPSC ring buffer (capacity power of two, indices padded to 128 bytes for M1) carries bytes from the I/O thread to the sequencer thread in live mode. In replay and test modes the engine is called directly, single threaded. The ring is a small, separately property tested component; nothing inside core depends on it.

## 3. Verification design

### 3.1 Invariants (checked after every event in test builds)

- Book shape: bids strictly descending, asks strictly ascending, no crossed book outside auction state, level aggregates equal the sum of resting orders, FIFO order within level matches sequence order.
- Conservation per order: entered quantity equals open plus executed plus canceled, never negative, monotone transitions only.
- Trade legality: every execution at a price no worse than the contemporaneous opposite best; no trade through of a better displayed level; buyer price >= seller price always.
- Output integrity: every fill references live orders, every cancel a live order, ids never reused within a session.

### 3.2 Deterministic simulation testing

A seeded generator produces randomized order flow over all order types with configurable mixes, including adversarial schedules (crossed pegs, stop cascades, iceberg refills under sweeps, auction boundary races). The harness runs the engine with full invariant checking, and on failure shrinks by delta debugging the input stream (prefix bisection then event removal passes) to a minimal reproducer written to disk with its seed. Fault injection at the boundary: duplicate delivery before the sequencer, malformed frames, truncated log then resume, crash restart with replay, snapshot restore versus full replay hash equality.

### 3.3 Fidelity against real NASDAQ data

gavel-extract streams a real TotalView-ITCH 5.0 day (curl into zlib into parser, nothing large touches disk) and emits compact per symbol gavel input streams plus the venue's own execution record. The fidelity validator then checks, for displayed liquidity on chosen liquid symbols:

- replaying adds, cancels, deletes, and replaces through the gavel book reproduces NASDAQ's book exactly (level by level comparison at sampled checkpoints);
- NASDAQ's executions are consistent with price time priority given the reconstructed book: no execution at a price while a better same side displayed order rested, and same level executions occur in FIFO order;
- counterfactual aggressors: for each venue execution, synthesize the implied incoming order into gavel and verify gavel fills the same resting order for the same quantity.

Documented caveats: hidden interest produces P trades we cannot attribute, cross trades and broken trades are excluded, odd lot and LULD edge cases are counted and skipped. The fidelity report states exact coverage numbers.

### 3.4 Conventional layers

Unit tests (doctest, vendored single header), property tests on the book and auction against brute force oracles, fuzzing of the ITCH parser and inbound frame decoder (libFuzzer via homebrew LLVM if available, otherwise the DST generator in mutation mode), ASan and UBSan jobs in CI.

## 4. Benchmark design

### 4.1 What is measured

- Per event engine latency: time from event dispatch to last output byte, single threaded, measured with cntvct_el0 (41.7 ns resolution on M1, stated everywhere) and with kperf PMU cycle counts when run with sudo.
- Latency under load, open loop: events carry scheduled arrival times from a constant rate schedule; latency is completion minus scheduled arrival. This makes queueing delay visible and is immune to coordinated omission by construction. Closed loop numbers are reported alongside, labeled as such, to show the difference.
- Throughput: full stream processing rate on synthetic flow and on real ITCH derived flow, which has realistic add to cancel ratios and touch locality.
- Hardening matrix: unchecked, contracts style precondition assertions on the hot path, libc++ hardening fast and extensive modes. Same benchmarks, reported as tail deltas. This is the C++26 era question with actual numbers.

### 4.2 Methodology rules

- Full distributions always: p50, p90, p99, p99.9, p99.99, max, from an HDR style log linear histogram. Never a mean, never best of N.
- Environment captured into every result file: sysctl hardware dump, build flags, compiler version, thermal state before and after, run to run variance across 10 runs.
- macOS honesty: no core pinning exists, no frequency pinning exists, P versus E core placement is OS controlled. Mitigations: high QoS class, quiet machine, variance reporting, and outlier runs shown rather than dropped. The benchmark report includes a section on exactly what a Linux x86_64 deployment would change (isolcpus, TSC, huge pages, NUMA, kernel bypass ingress) and which numbers should and should not move.
- Cache line constant is 128 on this machine; Apple clang's interference size constant says 64 and is wrong for M1, which the code documents and overrides.
- Published exchange figures (SIX 37 us round trip including network, NASDAQ sub 40 us claims) appear as context with the explicit statement that engine only numbers are not comparable to door to door numbers.

## 5. Failure modes considered

- ITCH edge messages (crosses, broken trades, market wide circuit breakers): excluded with counters, never silently dropped.
- Ladder anchor drift on volatile symbols: re anchoring is a between events operation with a determinism preserving rule (anchor moves are driven by the sequenced stream itself).
- Pool exhaustion: bounded pools with explicit reject behavior, tested.
- Malformed input: every frame validated before dispatch; fuzzing targets the validator.
- Timer coarseness on M1: medians below resolution are reported via PMU cycles and batched timing, never extrapolated.
- Disk: 2.7 GiB free on host; all real data work streams, nothing above ~300 MB persisted.

## 6. Success metrics

- Zero invariant violations across at least 10^9 DST events.
- Bit identical output hashes across 100 runs and across optimization levels.
- Fidelity: 100% book reconstruction match at checkpoints on selected symbols; priority violation rate zero for displayed flow modulo documented exclusions; counterfactual aggressor agreement rate reported.
- Performance: honest measured numbers with full distributions; the target to validate (not to claim in advance) is sub microsecond p99 per event on real derived flow and several million events per second throughput on one P core.
- Hardening study: quantified p50/p99 cost of each hardening level.

## 7. Security considerations

Inbound frames are length checked and field validated before touching engine state; all array indexing in release builds goes through bounds checked accessors in hardened configurations; ids are session scoped so replayed logs cannot collide; fuzzers run in CI. No network exposure exists in scope, but the validator boundary is treated as hostile anyway since that is where a real exchange gets attacked.

## 8. Roadmap beyond scope

Multicast egress with MoldUDP64 framing, OUCH wire compatibility, multi engine symbol sharding behind one sequencer, persistent log with torn write detection, a Linux x86_64 benchmark port with isolcpus methodology, and an MBO backtester consuming the engine for fill simulation.

## 9. Design decisions and tradeoffs

The fidelity check is scoped to displayed liquidity because public ITCH does not show aggressors and hidden interest pollutes priority checks. Fidelity claims carry exact exclusion counters, the counterfactual aggressor test reports an agreement rate rather than a boolean pass, and the docs state what cannot be verified from public data.

Determinism of a single threaded loop still has real failure modes: unordered container iteration leaking into output, float contamination, clock reads, address dependent behavior (pointer keyed maps), uninitialized padding in hashed output, and divergence between snapshot restore and replay. The O0 versus O3 hash check and ASLR rerun check exist because address dependence and padding bugs survive single run tests.

Pegs, stops, and auctions interacting expand the state space, which is why the DST generator has adversarial modes targeting those interactions and shrunk reproducers become regression tests.

M1 numbers are not production latency and the design does not claim them as such. The benchmarks aim for correct methodology, real distributions, documented platform limits, and an account of what changes on Linux x86_64.

Scope is managed with a strict build order and hard gates. Gate 1 core book and limit/market/IOC/FOK with invariants and determinism tests. Gate 2 DST harness. Gate 3 ITCH extract and fidelity. Gate 4 advanced order types. Gate 5 auctions. Gate 6 benchmarks and hardening study. Each gate leaves a shippable project; advanced types get cut before verification does.

Half tick prices invite off by one bugs, mitigated with strong types (Price wraps the half tick int, DisplayPrice refines it, construction validates parity) plus property tests over the conversion boundary.

The core has no third party dependencies, which keeps the determinism claims auditable and the build reproducible. doctest is vendored as one header with its license; the histogram is small and tested against a reference implementation in Python.
