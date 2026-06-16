# gavel technical deep dive

This document goes below the architecture into the parts that are subtle: the order
book data structure, the matching priority rules, the peg reprice fixed point, the
stop cascade, the auction cross algorithm, the determinism mechanism, and the
deterministic simulation testing harness.

## Order book

Each symbol side is a contiguous array of price levels indexed by offset from a
session anchor, with two bitsets over the level array: one for any occupancy, one
for non peg occupancy. Best price is a cached index refreshed on the rare event
that the touch empties; next worse walks use a word at a time scan over the bitset
(countr_zero / countl_zero). Each level holds an intrusive doubly linked FIFO of
order nodes plus aggregate quantity and counts. Cancel by id is O(1): id to pool
index, unlink, update aggregates and occupancy.

Non displayed interest (hidden midpoint pegs) and not yet resting interest (parked
pegs, pending stops, queued market orders) live in per symbol sequence ordered
vectors that hold order ids, not pool indices. Holding ids means a released and
reused pool slot can never be mistaken for the original order. These vectors are
cleaned lazily: a dead id is zeroed in place and the vector is compacted once dead
entries exceed half its length.

Prices more than the ladder window from the anchor are rejected as out of band,
which is the documented price collar and also bounds a market sweep structurally:
resting orders only exist inside the window, so a market order cannot trade beyond
it.

## Matching priority

Continuous matching is price then time, displayed before hidden at the same price,
with one twist for midpoint liquidity. At each step the engine computes the best
displayed opposite price A and the midpoint M (defined only when both displayed
sides exist). Because displayed prices are even and M is odd, M is never equal to
A. If M is strictly better for the incoming order than A, eligible hidden midpoint
orders execute at M first; otherwise displayed orders at A execute first. Within a
class, FIFO by sequence.

Order types and their exact rules are in `semantics.md`, which is
normative: limit, market, IOC, FOK, iceberg, primary and market and midpoint pegs,
stop and stop limit, self match prevention with three policies, and opening and
closing auctions. FOK fillability is computed against the displayed book only,
before any fill, and the order's SMP policy is coerced to cancel resting during the
sweep so the all or none promise cannot break midway.

## Pegs and the reprice fixed point

Pegs reference the best displayed non peg price on the relevant side; they never
reference other pegs for their target, which prevents mutual repricing loops.
After every book changing event the engine drains a reprice pass: for each touched
symbol it recomputes pegs in sequence order, and a peg whose effective price
changes moves to the tail of its new level (losing time priority) and emits
Repriced. A peg with no reference parks.

One subtlety the verification harness pinned down precisely: a peg's computed price
is clamped one tick passive of the opposite displayed touch so it is never
marketable, and that clamp must reference the full displayed best, including other
pegs, not just the non peg best. Pegs do not reference other pegs for their target,
but they must not cross other displayed pegs either, and a clamp against only the
non peg best lets two pegs cross. An attempt to weaken the clamp (to make the
reprice pass cheaper) was caught by the DST invariant checker as a crossed book on
the first run; the clamp on the full best is load bearing. lessons-learned.md walks
through this in more detail.

The cost of the reprice pass is O(live pegs) per book changing event, which the peg
heavy DST preset exposes directly: as the resting peg set grows, checked throughput
falls roughly inversely. This is an honestly measured scaling property, not hidden.

## Stops and the trigger cascade

Untriggered stops are not in the book and are invisible to market data. After the
current event's matching completes, every stop whose trigger price was crossed by a
print this event (including auction and midpoint prints) converts in sequence order
and is processed as a new incoming order, keeping its original id. A converted stop
that trades can trigger further stops; the cascade runs to completion before the
next sequenced event. A real bug here, found during development, was a triggered
stop whose market remainder was canceled but never released, leaking ids and digest
state and allowing a double cancel; the fix and its shrunk reproducer are regression
tested.

## Auction cross

The crossing price maximizes executable volume; ties minimize absolute imbalance;
remaining ties choose the price closest to the last trade reference, then the lower
price. Demand at price p is market buys plus limit buys priced at or above p; supply
is market sells plus limit sells priced at or below p; executable volume is the
minimum.

Demand is non increasing in price and supply non decreasing, so the candidate scan
is a prefix sum sweep over the sorted distinct candidate prices: aggregate displayed
open quantity by price, sort the candidate prices once, then sweep maintaining
running demand (subtracting buys as price rises) and supply (adding sells as price
rises). This is O(C log C) in the number of distinct candidate prices, replacing an
earlier O(C^2) scan that recomputed both sums for every candidate. The tie break is
a total order over prices, so the sweep selects the same price the quadratic scan
did; the change was verified byte identical on the auction DST presets and on the
real NASDAQ close cross.

Allocation walks both sides simultaneously in priority order (market first, then
prices strictly better than the cross, then the cross price, FIFO within each
class), pairing fills at the cross price. The whole algorithm is property tested
against a brute force oracle.

## Determinism mechanism

The Emitter is the single output sink. `emit` writes a two byte header (type and
payload size) then the packed payload, folding every byte into a running FNV-1a 64
hash before appending it to the drainable buffer. Packed structs and a fixed field
order mean there is no uninitialized padding in the hashed bytes. The book digest is
an incremental xor of a per order mix of id, price, open quantity, and state,
updated on every order change, so a full book equality check is one 64 bit compare.

Snapshot save writes a header, per side ladder bases, and all live orders (ladder
orders in FIFO walk order, the rest in id order), and verifies the reconstructed
digest on load. Snapshot plus tail replay producing the same hash as full replay is
a test, which catches any state the snapshot forgot to carry.

## Deterministic simulation testing

A seeded splitmix64 generator (chosen over std::mt19937 so the stream is bit
identical across platforms) produces randomized flow over all order types with
configurable mixes and adversarial presets: peg heavy, stop cascade, iceberg sweep,
auction mix, tight book. The DST driver runs the engine with invariant checks after
every event and an independent shadow book reconstructed from the output stream; on
divergence it shrinks the input by delta debugging (failing prefix, then chunk
removal at decreasing sizes) to a minimal reproducer written to disk with its seed.
Fault injection at the boundary covers duplicate delivery, malformed frames, and
snapshot restore versus full replay equality.

The invariants checked: book shape (bids descending, asks ascending, no crossed book
in the open state, level aggregates equal the sum of resting orders, FIFO matches
sequence order), per order conservation (entered equals open plus executed plus
canceled, never negative, monotone), trade legality (every execution at a price no
worse than the contemporaneous opposite best, no trade through, buyer price at least
seller price), and output integrity (every fill and cancel references a live order,
ids never reused).
