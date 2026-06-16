#pragma once
#include <cstdint>
#include <cstddef>

namespace gavel {

// Apple clang reports 64 here; M1 cache lines are 128 bytes.
inline constexpr std::size_t kCacheLine = 128;

using Seq = std::uint64_t;
using OrderId = std::uint64_t;
using Ts = std::uint64_t;
using Qty = std::int32_t;
using SymbolIdx = std::uint16_t;
using Participant = std::uint16_t;

// Price in half ticks; displayed prices are even, midpoint executions may be odd.
struct Price {
  std::int32_t v{0};
  constexpr Price() = default;
  constexpr explicit Price(std::int32_t hv) : v(hv) {}
  static constexpr Price from_ticks(std::int32_t t) { return Price{t * 2}; }
  constexpr bool valid() const { return v > 0; }
  constexpr bool displayable() const { return v > 0 && (v & 1) == 0; }
  constexpr bool operator==(const Price&) const = default;
  constexpr auto operator<=>(const Price&) const = default;
};
inline constexpr Price kNoPrice{0};

enum class Side : std::uint8_t { buy = 0, sell = 1 };
constexpr Side opposite(Side s) { return s == Side::buy ? Side::sell : Side::buy; }

enum class OrderKind : std::uint8_t {
  limit = 0, market = 1, peg_primary = 2, peg_market = 3, peg_mid = 4,
  stop = 5, stop_limit = 6
};

enum class Tif : std::uint8_t { day = 0, ioc = 1, fok = 2 };

enum class SmpPolicy : std::uint8_t { none = 0, cancel_resting = 1, cancel_incoming = 2, cancel_both = 3 };

enum class SessionState : std::uint8_t { halted = 0, pre_open = 1, open = 2, post_close = 3, ended = 4 };

enum class ClockAction : std::uint8_t { session_start = 0, open_cross = 1, close_cross = 2, session_end = 3 };

enum class Reason : std::uint8_t {
  none = 0, bad_symbol, bad_price, bad_qty, bad_kind, bad_tif, bad_offset,
  unknown_target, wrong_state, peg_in_auction, tif_in_auction, pool_exhausted,
  smp, ioc_expired, fok_unfilled, no_liquidity, collar, auction_unfilled,
  session_end, user_cancel, replaced
};

struct Config {
  std::uint32_t num_symbols{1};
  // Ladder window in half ticks per side structure.
  std::uint32_t ladder_window{8192};
  std::uint32_t initial_order_capacity{1 << 16};
  std::uint32_t max_orders{1u << 26};
  std::uint64_t checkpoint_interval{100000};
};

}  // namespace gavel
