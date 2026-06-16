# Lessons learned

## The verification harness earns its keep by rejecting your own changes

The most valuable moment in this project was not a feature landing. It was the DST
harness rejecting a plausible optimization. The peg price clamp references the full
displayed best; changing it to the non peg best looked like a clean win that would
also unlock a cheaper reprice pass. The invariant checker flagged a crossed book on
the first run across several presets and seeds, in under a second, before the change
could ship. Pegs do not reference other pegs for their target price, but they must
not cross other displayed pegs, so the clamp is load bearing. A reviewer skimming
the diff might have waved it through. The harness did not. That is what the
verification harness is for.

## Honest metrics beat green checkmarks

The fidelity tool originally printed PASS on an early session slice and FAIL on the
full trading day, and the failure was not understood. The temptation is to tune the
thresholds until it says PASS. The right move was the opposite: instrument the
failures, find that every time priority divergence was the venue executing an order
around eight deep in our displayed queue (exactly what non displayed liquidity ahead
produces), and split the report into the guarantees public ITCH can verify
(displayed price priority, counterfactual aggressor) versus the metrics it cannot
(order level time priority, book agreement during marketable add windows). The tool
now reports rates with the caveat, and PASS means zero violations of the guarantees
that are actually verifiable. A scoped claim you can defend beats a broad claim you
cannot.

## Determinism is a design constraint, not a test you add later

Reaching bit identical output across optimization levels required deciding, up
front, that core reads no clock, allocates nothing on the event path, contains no
float, and never lets an unordered container or an address reach the output. Each of
those is a real bug class (padding in hashed structs, pointer keyed map iteration,
address dependent tie breaks) that a single run test would miss. The O0 versus O3
hash check and the ASLR rerun check exist specifically to catch them. Bolting
determinism onto an engine that already reads clocks and allocates freely would have
meant a rewrite.

## Sizing a soak honestly means measuring per workload first

The first soak attempt used a flat event budget per preset and died at the session
limit. The reason was a real scaling property: the peg heavy and tight book presets
grow the live order set roughly linearly, and the reprice and book passes cost grows
with it, so those presets dominate wall clock unpredictably. The fix was to measure
each preset's throughput, then size event counts per preset and split the soak into a
dense check every event tier, a check every thousand tier, and a bulk tier. Picking a
bounded size that completes and reporting the real event count is more honest than
claiming a billion events that never finished.

## Apple Silicon benchmarking requires saying what you cannot measure

The host has no core pinning, no frequency control, OS decided P versus E core
placement, a 41.7 ns timer, and no kperf PMU access without root. Every one of those
limits is stated in the benchmark output itself, so each result is self describing.
The benchmark report names exactly what a Linux x86_64 deployment with isolated
cores, a stable TSC, huge pages, and kernel bypass ingress would change, and which
numbers should and should not move. Honest methodology on modest hardware is more
convincing than invented nanoseconds on a laptop.

## Dependency free was the right call for an auditable correctness claim

The core has no third party dependencies. doctest is one vendored header for tests;
the histogram is a small implementation tested against a reference. This keeps the
build trivially reproducible and the determinism claim auditable: there is no
hidden allocation or hidden global state from a library to reason about. The cost is
writing a histogram and a hash by hand, which is small next to the benefit of a core
a reviewer can read top to bottom.
