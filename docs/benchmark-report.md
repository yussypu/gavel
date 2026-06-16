# gavel benchmark report

Every number here was measured on the host described below. None is extrapolated,
and none is a best of N. Where the timer resolution or the platform limits the
measurement, that is stated next to the number.

## Host and method

- MacBookAir10,1, Apple M1, 4 performance cores plus 4 efficiency cores, 8 GiB.
- Apple clang 17, C++23, Release build, `-O3 -DNDEBUG`, GAVEL_HARDENING=1,
  libc++ hardening mode 2 (the default for this build).
- Timer: cntvct_el0 at 24 MHz, so 41.7 ns per tick. Single shot readings at or
  below that tick are quantized; the batched rows give sub resolution means.
- PMU: kperf fixed counters need root, which is not available here, so cycles per
  event are not measured. The benchmark prints this on every run.
- macOS has no core pinning and no frequency control. The process requests the
  user_interactive QoS class, but the OS decides performance versus efficiency core
  placement. This shows up as run to run variance, which is reported, not hidden.
- Histogram: a log linear HDR style histogram, tested against a reference. Full
  percentiles always (p50, p90, p99, p99.9, p99.99, max), never a mean alone.
- Load generation: closed loop times each `on_msg` call. Open loop assigns each
  event a scheduled arrival from a constant rate and measures completion minus
  scheduled arrival, which makes queueing visible and is immune to coordinated
  omission by construction. Both are reported, labeled.

## Throughput (synthetic workloads, 5,000,000 events, 10 reps)

| workload | median M events/s | min | max | run to run spread |
| --- | --- | --- | --- | --- |
| add_cancel (heavy add then cancel near touch, ~10% marketable) | 9.58 | 9.37 | 9.60 | 2.4% |
| sweep_heavy (deep book, bursts of large marketable sweeps) | 4.64 | 3.83 | 4.79 | 20.8% |
| mixed_realistic (50% add, 25% cancel, 10% reduce, 10% replace, 5% marketable) | 6.41 | 4.18 | 6.70 | 39.3% |

The wide spread on the mixed and sweep workloads is the P versus E core placement
and frequency variance on an unpinned laptop. It is reported as the spread of ten
runs rather than smoothed away. add_cancel is the most stable because it is the most
cache local.

## Throughput on real NASDAQ ITCH derived flow

Replaying the extracted AAPL stream from 2020-01-30 (361,642 events, realistic add
to cancel ratios and touch locality): median 7.05 M events/s over 5 reps, spread
2.9%.

## Per event latency (nanoseconds)

add_cancel, 1,000,000 events:

| mode | p50 | p90 | p99 | p99.9 | p99.99 | max |
| --- | --- | --- | --- | --- | --- | --- |
| closed loop, per event | 83 | 167 | 334 | 458 | 1752 | 15250 |
| closed loop, batched x1024 (per batch mean) | 105 | 111 | 119 | 134 | 134 | 134 |

p50 of 83 ns sits at two timer ticks and is quantized; the batched mean of 105 ns is
the honest sub resolution central number. The tail (p99 334 ns, p99.99 1752 ns)
reflects real work plus OS noise on an unpinned core.

Real ITCH AAPL flow, per event: p50 83 ns (quantized), p90 290 ns, p99 374 ns,
batched mean 139 ns.

sweep_heavy, per event: p50 125 ns, p90 418 ns, p99 916 ns, p99.9 1368 ns. Heavier
because each marketable order walks multiple levels.

## Open loop, coordinated omission aware

For add_cancel, scheduling arrivals at fractions of measured max throughput:

| target rate | achieved | p50 | p99 | p99.9 |
| --- | --- | --- | --- | --- |
| 4.77 M/s (50%) | 4.77 M/s | 83 | 580 | 60672 |
| 7.63 M/s (80%) | 7.63 M/s | 209 | 5152 | 17024 |
| 9.06 M/s (95%) | 8.50 M/s | saturated | saturated | saturated |

At 95% of max the single threaded harness cannot keep up (achieved 8.50 against a
9.06 target), so arrivals queue and the latency column reflects queueing, not engine
service time. This is labeled in the output rather than dropped. It is the honest
shape of a closed system pushed near its service rate.

## Hardening cost study

Same add_cancel workload, GAVEL_HARDENING=0 (no precondition checks) versus
GAVEL_HARDENING=1 (hot path preconditions enforced):

| build | per event p50 | p90 | p99 | batched mean |
| --- | --- | --- | --- | --- |
| HARDENING=0 | 83 | 209 | 458 | 135 |
| HARDENING=1 | 83 | 167 | 334 | 105 |

The hardened build is not measurably slower; here it is marginally faster, which is
within run to run variance. The honest conclusion is that on this hot path, at this
machine's timer resolution and noise floor, the precondition checks have no
detectable tail cost. The run to run variance from unpinned cores exceeds the
hardening delta, so a smaller cost cannot be resolved here. Resolving it would need
PMU cycle counts on isolated cores, which is the Linux measurement below.

## What would differ on Linux x86_64 with isolated cores and kernel bypass

- Isolated cores (isolcpus, nohz_full) and a pinned thread remove the P versus E
  placement variance that produces the 20 to 40% throughput spread here. The
  distributions would tighten and the tail would drop, because most of the tail on
  this host is OS scheduling noise, not engine work.
- A stable invariant TSC plus rdtscp gives sub nanosecond timer resolution, so the
  p50 would be a real number instead of a quantized 83 ns, and the batched rows
  would not be needed to see below the tick.
- PMU access (perf_event_open) gives cycles and instructions per event, turning the
  hardening study from "no detectable difference" into a precise cycle delta per
  precondition, which is the number the C++ contracts discussion actually wants.
- Huge pages and a presized pool remove TLB misses on the order slab; on this host
  the pool is presized but pages are 16 KiB and not huge.
- Kernel bypass ingress (a user space NIC stack) changes the door to door latency,
  not the engine service time measured here. The engine numbers are service time
  only and are not comparable to published door to door figures such as SIX at
  roughly 37 microseconds round trip including network or NASDAQ sub 40 microsecond
  claims. Those include the network and the matching engine; these measure only the
  matching engine. That distinction is stated so the numbers are not misread as a
  claimed win.

## Reproducing

```
build/bench/gavel-bench throughput --reps 10 --json results/throughput.jsonl
build/bench/gavel-bench latency --json results/latency.jsonl
build/bench/gavel-bench throughput --replay data/20200130/AAPL.gvl --reps 5
build/bench/gavel-bench latency --replay data/20200130/AAPL.gvl
```

Each JSON line carries the full host and build environment, so a result file is self
describing.
