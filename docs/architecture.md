# gavel architecture

gavel is a deterministic exchange matching engine in C++23. The design invariant
the whole system is built around is this: the sequenced input stream fully
determines every output byte. Everything else is a consequence of, or a test of,
that one property.

## Pipeline

```
 inbound (order entry, clock events)
        |
   [ sequencer ]  assigns seq no + logical timestamp
        |
   sequenced input log  (single source of truth, replayable)
        |
   [ matching engine ]  single threaded, no clock, no syscalls, no allocation on the event path
        |
   output event stream  (acks, executions, book deltas, periodic state hashes)
```

Every downstream view (market data, order acks, snapshots) is a pure function of
the sequenced input. The engine consumes one message at a time and emits a typed,
length prefixed event stream through a single sink that hashes every byte.

## Components

- `include/gavel/` core: types, order book, matching engine, auction cross,
  output emitter. Freestanding, no I/O, no clock reads, no third party deps.
  - `types.hpp` strong Price (half ticks) and the order, side, tif, state enums.
  - `book.hpp` per symbol ladder: contiguous price levels over a window, bitset
    occupancy for best and next-worse walks, intrusive FIFO per level, plus
    sequence ordered auxiliary lists for pegs, hidden midpoints, stops, and queued
    market orders.
  - `pool.hpp` slab allocator for order nodes, indexed dense from 1.
  - `idmap.hpp` open addressing map from order id to pool index.
  - `output.hpp` the Emitter and the packed event structs.
  - `engine.hpp` the engine interface and per event scratch buffers.
- `src/engine.cpp` dispatch, validation, session and clock handling, snapshot
  save and restore. `src/match.cpp` continuous matching, icebergs, pegs and the
  reprice pass, stops and the trigger cascade, and the auction cross.
- `itch/` NASDAQ TotalView-ITCH 5.0 parser, the per symbol extractor that turns a
  real ITCH day into a gavel input stream plus a venue execution sidecar, and the
  fidelity validator.
- `verify/` invariant checker, an independent shadow book reconstructed from the
  output stream, the seeded flow generator with adversarial presets, and the DST
  driver with a delta debugging shrinker.
- `bench/` HDR style log linear histogram, a cntvct_el0 timer, a kperf PMU wrapper,
  synthetic and replay workloads, and the benchmark driver with environment capture.
- `tools/` CLI binaries: gavel-replay, gavel-sim, gavel-extract, gavel-fidelity.

## Numerics and identity

- Prices are int32 in half ticks. Displayed prices are even (whole ticks); odd
  values occur only as midpoint execution prices. There is no floating point in
  core. A strong Price type carries the parity proof.
- Quantities are int32 shares. Order ids are uint64 equal to the sequence number of
  the message that created the order, so they are unique per session and never
  reused, which removes a class of replay aliasing bugs.
- Symbols are uint16 indices into a table fixed at session start.

## Determinism enforcement

1. Core never reads a clock, never does I/O, never allocates on the event path
   (pools grow only between events), contains no floats, and never lets unordered
   iteration or an address influence output.
2. All ordering ties resolve by sequence number; symbol scoped passes run in symbol
   index order.
3. Every output byte flows through the Emitter, which maintains a running FNV-1a 64
   hash. Every checkpoint interval the engine emits a StateHash carrying the output
   hash and an incrementally maintained book digest (xor of a per order mix,
   updated O(1) on every order change).
4. The test suite proves identical output hashes across repeated runs, across
   optimization levels O0/O2/O3, across address space randomization reruns, and
   between snapshot-plus-tail replay and full replay.

## Boundary

In live mode a single SPSC ring buffer (power of two capacity, indices padded to
128 bytes for the M1 cache line) carries bytes from the I/O thread to the engine
thread. In replay and test modes the engine is called directly, single threaded.
The ring is property tested in isolation; nothing in core depends on it.

## What is deliberately out of scope

Network transport (multicast, kernel bypass) beyond clean interfaces, multi
instrument risk checks, persistence beyond the sequenced log, and strategy logic.
The sharding interface (many symbols behind one sequencer) is named in the roadmap
but not built.
