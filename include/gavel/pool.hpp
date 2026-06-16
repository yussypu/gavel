#pragma once
#include <cstdint>
#include <vector>
#include "gavel/types.hpp"
#include "gavel/idmap.hpp"

namespace gavel {

inline constexpr std::uint32_t kNil = 0xffffffffu;

// Resting or pending order state; intrusive links are pool indices.
struct Order {
  OrderId id{0};
  std::uint32_t prev{kNil};
  std::uint32_t next{kNil};
  Price price;          // current resting price; 0 when parked or untriggered
  Price cap;            // peg limit cap or stop limit price
  Price trigger;        // stop trigger price
  Qty display_open{0};  // open displayed quantity (all of it for non icebergs)
  Qty reserve{0};       // iceberg hidden reserve
  Qty display_size{0};  // iceberg display refill size, 0 for plain orders
  Qty cum_executed{0};
  std::int32_t peg_offset{0};
  SymbolIdx symbol{0};
  Participant participant{0};
  Side side{Side::buy};
  OrderKind kind{OrderKind::limit};
  Tif tif{Tif::day};
  SmpPolicy smp{SmpPolicy::none};
  std::uint8_t state{0};  // OrderState

  Qty open() const { return display_open + reserve; }
  bool iceberg() const { return display_size > 0; }
};

enum class OrderState : std::uint8_t {
  dead = 0, resting = 1, parked = 2, pending_stop = 3, queued_market = 4
};

inline OrderState ostate(const Order& o) { return static_cast<OrderState>(o.state); }
inline void set_state(Order& o, OrderState s) { o.state = static_cast<std::uint8_t>(s); }

// Slab pool with freelist; grows only between events via reserve discipline.
class OrderPool {
 public:
  explicit OrderPool(std::uint32_t initial) { slab_.reserve(initial); }

  std::uint32_t alloc() {
    if (free_ != kNil) {
      const std::uint32_t i = free_;
      free_ = slab_[i].next;
      slab_[i] = Order{};
      return i;
    }
    slab_.emplace_back();
    return static_cast<std::uint32_t>(slab_.size() - 1);
  }

  void release(std::uint32_t i) {
    slab_[i].state = 0;
    slab_[i].next = free_;
    free_ = i;
  }

  Order& operator[](std::uint32_t i) { return slab_[i]; }
  const Order& operator[](std::uint32_t i) const { return slab_[i]; }
  std::size_t capacity() const { return slab_.capacity(); }
  std::size_t size() const { return slab_.size(); }

 private:
  std::vector<Order> slab_;
  std::uint32_t free_{kNil};
};

}  // namespace gavel
