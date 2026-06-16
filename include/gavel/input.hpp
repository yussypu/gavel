#pragma once
#include <cstring>
#include <type_traits>
#include "gavel/types.hpp"

namespace gavel {

enum class MsgType : std::uint8_t { enter = 1, cancel = 2, reduce = 3, replace = 4, clock = 5 };

// Fixed 48 byte sequenced input record; the stream is the source of truth.
struct InputMsg {
  Seq seq{0};
  Ts ts{0};
  OrderId target{0};
  MsgType type{MsgType::enter};
  Side side{Side::buy};
  OrderKind kind{OrderKind::limit};
  Tif tif{Tif::day};
  SymbolIdx symbol{0};
  Participant participant{0};
  Qty qty{0};
  std::int32_t price{0};
  std::int32_t aux{0};
  std::uint8_t flags{0};
  std::uint8_t clock_action{0};
  std::uint16_t pad{0};

  SmpPolicy smp() const { return static_cast<SmpPolicy>(flags & 0x3); }
  Price limit_price() const { return Price{price}; }
  Price aux_price() const { return Price{aux}; }
};
static_assert(sizeof(InputMsg) == 48);
static_assert(std::is_trivially_copyable_v<InputMsg>);

inline InputMsg make_enter(Seq seq, Ts ts, SymbolIdx sym, Side side, OrderKind kind,
                           Tif tif, Qty qty, std::int32_t price, std::int32_t aux = 0,
                           Participant part = 0, SmpPolicy smp = SmpPolicy::none) {
  InputMsg m;
  m.seq = seq; m.ts = ts; m.type = MsgType::enter; m.side = side; m.kind = kind;
  m.tif = tif; m.symbol = sym; m.participant = part; m.qty = qty; m.price = price;
  m.aux = aux; m.flags = static_cast<std::uint8_t>(smp);
  return m;
}

inline InputMsg make_cancel(Seq seq, Ts ts, OrderId target) {
  InputMsg m;
  m.seq = seq; m.ts = ts; m.type = MsgType::cancel; m.target = target;
  return m;
}

inline InputMsg make_reduce(Seq seq, Ts ts, OrderId target, Qty by) {
  InputMsg m;
  m.seq = seq; m.ts = ts; m.type = MsgType::reduce; m.target = target; m.qty = by;
  return m;
}

inline InputMsg make_replace(Seq seq, Ts ts, OrderId target, Qty qty, std::int32_t price) {
  InputMsg m;
  m.seq = seq; m.ts = ts; m.type = MsgType::replace; m.target = target;
  m.qty = qty; m.price = price;
  return m;
}

inline InputMsg make_clock(Seq seq, Ts ts, ClockAction action) {
  InputMsg m;
  m.seq = seq; m.ts = ts; m.type = MsgType::clock;
  m.clock_action = static_cast<std::uint8_t>(action);
  return m;
}

}  // namespace gavel
