#pragma once
#include <cstring>
#include <vector>
#include <type_traits>
#include "gavel/types.hpp"
#include "gavel/hash.hpp"

namespace gavel {

enum class EventType : std::uint8_t {
  accepted = 1, rejected = 2, canceled = 3, reduced = 4, replaced = 5,
  executed = 6, repriced = 7, refilled = 8, stop_triggered = 9,
  auction_result = 10, session_event = 11, state_hash = 12
};

#pragma pack(push, 1)
struct EvAccepted {
  OrderId id; Ts ts; std::int32_t price; Qty qty; std::int32_t aux;
  SymbolIdx symbol; Side side; OrderKind kind; Tif tif; std::uint8_t display;
};
struct EvRejected { Seq seq; Ts ts; Reason reason; };
struct EvCanceled { OrderId id; Ts ts; Qty qty_canceled; Reason reason; };
struct EvReduced { OrderId id; Ts ts; Qty qty_reduced; Qty remaining; };
struct EvReplaced { OrderId old_id; OrderId new_id; Ts ts; };
struct EvExecuted {
  OrderId resting; OrderId incoming; Ts ts; std::int32_t price; Qty qty;
  Qty resting_remaining; std::uint8_t flags;
  static constexpr std::uint8_t kAuction = 1, kHidden = 2;
};
struct EvRepriced { OrderId id; Ts ts; std::int32_t old_price; std::int32_t new_price; };
struct EvRefilled { OrderId id; Ts ts; Qty display_qty; };
struct EvStopTriggered { OrderId id; Ts ts; std::int32_t trigger_price; };
struct EvAuctionResult { Ts ts; std::int64_t matched_qty; std::int64_t imbalance; std::int32_t price; SymbolIdx symbol; };
struct EvSessionEvent { Ts ts; ClockAction action; };
struct EvStateHash { Ts ts; std::uint64_t events_processed; std::uint64_t out_hash; std::uint64_t book_digest; };
#pragma pack(pop)

// Single sink for all engine output; maintains the running hash over every byte.
class Emitter {
 public:
  template <typename Ev>
  void emit(EventType t, const Ev& ev) {
    static_assert(std::is_trivially_copyable_v<Ev>);
    static_assert(sizeof(Ev) <= 255);
    const std::uint8_t hdr[2] = {static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(sizeof(Ev))};
    append(hdr, 2);
    append(&ev, sizeof(Ev));
    ++count_;
  }
  std::uint64_t hash() const { return hash_; }
  std::uint64_t count() const { return count_; }
  const std::vector<std::uint8_t>& buffer() const { return buf_; }
  // Drains the buffer; the hash keeps accumulating across drains.
  void drain() { buf_.clear(); }
  void reserve(std::size_t n) { buf_.reserve(n); }
  // Snapshot restore only; continues the running hash from a saved point.
  void restore(std::uint64_t h, std::uint64_t c) { hash_ = h; count_ = c; buf_.clear(); }

 private:
  void append(const void* p, std::size_t n) {
    hash_ = fnv1a64(p, n, hash_);
    const auto* b = static_cast<const std::uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  std::vector<std::uint8_t> buf_;
  std::uint64_t hash_{kFnvOffset};
  std::uint64_t count_{0};
};

}  // namespace gavel
