# gavel engine semantics, normative

This document is the contract. Code implements exactly this; tests test exactly this. Deviations from real venue behavior are called out as Deviation notes.

## Numerics

- Price: int32 in half ticks. Displayed prices must be even (whole ticks). Odd values occur only as midpoint execution prices. Price 0 is invalid.
- Quantity: int32 shares, > 0.
- Order id: uint64, equal to the sequence number of the message that created the order (enter or replace). Ids are unique per session, never reused.
- Timestamps: uint64 logical nanoseconds carried by the sequenced stream. The engine never reads a clock.

## Input messages (48 byte fixed records)

Types: Enter, Cancel, Reduce, Replace, Clock.

- Enter: symbol, side, kind (limit, market, peg_primary, peg_market, peg_mid, stop, stop_limit), tif (day, ioc, fok), qty, price, aux, participant, flags (smp policy in bits 0..1).
  - limit: price = limit (even). aux = iceberg display size (0 means fully displayed).
  - market: price ignored (0). aux = 0.
  - peg_primary, peg_market: price = optional limit cap (0 means none), aux = passive offset in half ticks. Offset must be >= 0 and even for peg_primary, >= 2 and even for peg_market.
  - peg_mid: price = optional limit cap, aux = 0. Hidden.
  - stop: price = 0, aux = trigger price (even). Becomes market on trigger.
  - stop_limit: price = limit price (even), aux = trigger price (even). Becomes limit on trigger.
- Cancel: target id. Cancels remaining quantity (reason user_cancel).
- Reduce: target id, qty = shares to remove. Keeps queue priority. Reducing to zero or below remaining cancels the order (reason user_cancel).
- Replace: target id, new qty, new price. Semantics: cancel target, enter a new limit order with id = seq of the replace message, same symbol, side, participant, tif, iceberg display size; always loses time priority. Deviation: this is ITCH replace semantics (always new position), not OUCH (which can keep priority); chosen so fidelity replay of ITCH U messages is exact.
- Clock: action in {session_start, open_cross, close_cross, session_end}. Market wide.

Validation failures reject with a reason code and have no other effect. Sequence numbers must be strictly increasing; violation is a fatal stream error.

## Sessions

States: Halted (before session_start), PreOpen (after session_start), Open (after open_cross completes), PostClose (after close_cross completes, all day orders canceled with reason session_end), Ended.

- PreOpen: enters accepted for limit (incl. iceberg), market, stop, stop_limit. Pegs rejected (reason peg_in_auction). IOC and FOK rejected (reason tif_in_auction). No matching; the book may be crossed. Market orders queue FIFO per side, not in the ladder.
- Open: continuous trading per the matching rules below.
- PostClose and Halted and Ended: all order entry rejected (reason session_state). Cancels of live orders accepted in PreOpen and Open only.

## Continuous matching

Incoming marketable orders execute against the book; the resting side sets the execution price. Priority at each step:

1. Compute best displayed opposite price A (ladder best) and the midpoint M = (best_bid + best_ask) / 2 in half ticks, defined only when both displayed sides exist.
2. Eligible hidden midpoint orders (opposite side, cap allows M) execute at M before displayed orders at A if M is strictly better for the incoming order than A; otherwise displayed first. For an incoming buy, M < A means midpoint first. When M == A is impossible (M is odd, A is even).
3. Within displayed levels: FIFO by sequence. Within hidden midpoint orders: FIFO by sequence.
4. Each fill emits Executed with the resting order id, incoming order id, price, quantity, and the resting order's remaining quantity.

- limit: execute while marketable, then rest the remainder in the ladder (displayed; iceberg rests display portion, reserve held back).
- market: execute against displayed and eligible hidden interest until filled or the opposite book is exhausted; any remainder cancels (reason no_liquidity). Market orders never rest in Open. The ladder window bounds executable prices structurally: resting orders only exist inside it, so a market sweep cannot trade beyond the collar.
- IOC: as limit, remainder cancels (reason ioc_expired).
- FOK: compute total fillable quantity at or better than the limit against the displayed book only (hidden midpoint liquidity never counts toward fillability; this conservative rule keeps the all or none promise exact since midpoint eligibility shifts as the sweep moves the BBO), honoring SMP exclusions; if less than order quantity, cancel in full (reason fok_unfilled), else execute fully. A FOK order's smp policy is coerced to cancel_resting during matching so the all or none promise cannot be broken mid sweep.
- Self match prevention: when the next resting order to execute has the same participant id as the incoming order and the incoming order's smp policy is not none: cancel_resting cancels the resting order (reason smp) and matching continues; cancel_incoming cancels the incoming remainder (reason smp); cancel_both does both. Policy is taken from the incoming order.

## Icebergs

An iceberg rests its display size; the reserve is invisible to the book and to market data. When the displayed portion fully executes and reserve remains, the order refills min(display_size, reserve) at the tail of its price level (time priority lost on refill, matching NASDAQ), emitting Refilled. A reduce takes from reserve first, then display. Display refill happens immediately within the same event before further matching at that level continues. Deviation note: real venues differ on refill timing; this rule is documented and deterministic.

## Pegged orders

- References: best displayed non peg price on the relevant side. Pegs never reference other pegs (prevents mutual prop loops; Deviation: venues that allow peg to peg referencing accept the loop and break it with NBBO, which does not exist here).
- peg_primary (buy): ref = best non peg bid; pegged price = ref - offset. Sell mirror.
- peg_market (buy): ref = best non peg ask; pegged price = ref - offset (offset >= 1 tick keeps it from locking). Sell mirror.
- Cap: effective price = min(pegged, cap) for buys, max for sells, when cap != 0.
- A peg with no reference parks: it leaves the ladder and is unmatchable until a reference appears. Parking and unparking emit Repriced with price 0 for the parked side. A peg that prices at entry emits Repriced from 0 to its initial price after Accepted; a peg that parks at entry emits Accepted only.
- peg_mid: hidden, executes only at the current midpoint, parks when either displayed side is missing or cap excludes M.
- Repricing: after each sequenced event completes (including stop cascade), the engine drains a reprice pass: for each symbol whose non peg BBO changed, recompute all pegs of that symbol in sequence order; a peg whose effective price changes moves to the tail of its new level (time priority lost) and emits Repriced. The pass iterates until no peg moves; termination is guaranteed because peg moves never change non peg BBO.
- Pegs are never marketable on entry or reprice: a computed price that would cross the opposite displayed best is clamped to one tick passive of it. Deviation: simplification; documented.

## Stops

- Trigger rule: buy stop triggers when a trade prints at or above the trigger price; sell stop at or below. Triggers are evaluated against every execution price, including auction crosses and midpoint executions.
- Untriggered stops are not in the book and are invisible to market data.
- After the current event's matching completes, all triggered stops convert in sequence order (oldest first) and are processed as new incoming orders (market or limit). Their executions may trigger further stops; the cascade processes to completion before the next sequenced event. Each conversion emits StopTriggered then follows normal entry semantics, keeping the original order id.

## Auctions

Open cross and close cross, driven by Clock events.

- Eligible: all live limit orders in the ladder plus queued market orders. Stops do not participate (and cannot trigger during the cross; cross executions evaluate stop triggers after the cross completes, as one batch).
- Crossing price p* maximizes executable volume; ties minimize absolute imbalance; remaining ties choose the price closest to the reference price (the last trade; with no last trade there is no proximity preference); a final tie takes the lower price.
- Demand B(p) = market buys plus limit buys with price >= p; supply S(p) = market sells plus limit sells with price <= p; exec(p) = min(B(p), S(p)). Candidates are displayed limit prices where both sides overlap.
- Allocation at p*: market orders first, then limit orders strictly better than p*, then orders at p*, FIFO within each class; both sides walked simultaneously, fills paired and emitted as Executed with auction flag.
- After the cross: AuctionResult is emitted (price, matched volume, imbalance) per symbol with nonzero book or queue activity, in symbol index order, then unfilled market order remainders cancel (reason auction_unfilled). Open cross emits SessionEvent before the crosses and transitions to Open; close cross runs the crosses, cancels every remaining live order (reason session_end) in id order, transitions to PostClose, then emits SessionEvent.
- If no cross is possible (no overlap), AuctionResult emits with price 0 and volume 0, then market orders cancel, and the transition still happens.

## Determinism rules

1. Core has no clock reads, no I/O, no allocation on the event path (pools grow only between events), no floats, no output influenced by unordered iteration or addresses.
2. All ordering ties resolve by sequence number; symbol scoped passes run in symbol index order.
3. Every output byte flows through the Emitter, which maintains a running FNV 1a 64 hash. Every checkpoint_interval events the engine emits StateHash carrying the output hash and an incrementally maintained book digest (xor of mix(id, price, open qty) over all live orders, updated O(1) per change).
4. Required test results: identical output hashes across runs, across O0/O2/O3, across ASLR reruns, and snapshot plus tail replay equals full replay.

## Output events

Header per event: type byte, size byte, then payload struct (packed, no padding). Types: Accepted, Rejected, Canceled, Reduced, Replaced, Executed, Repriced, Refilled, StopTriggered, AuctionResult, SessionEvent, StateHash. Executed flags: bit0 auction, bit1 hidden_exec. Reasons enum is shared with input validation.

## Reject reasons

bad_symbol, bad_price (zero, odd displayed, out of collar window), bad_qty, bad_kind, bad_tif, bad_offset, unknown_target (cancel/reduce/replace of dead or foreign id), wrong_state (session_state), peg_in_auction, tif_in_auction, pool_exhausted, smp, ioc_expired, fok_unfilled, no_liquidity, collar, auction_unfilled, session_end. Cancels report reasons; rejects report reasons; both are hashed output.
