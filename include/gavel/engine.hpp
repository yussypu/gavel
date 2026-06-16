#pragma once
#include <cstdint>
#include <vector>
#include "gavel/types.hpp"
#include "gavel/input.hpp"
#include "gavel/output.hpp"
#include "gavel/book.hpp"
#include "gavel/pool.hpp"
#include "gavel/idmap.hpp"

namespace gavel {

// Deterministic single threaded matching engine; see docs/semantics.md.
class Engine {
 public:
  explicit Engine(const Config& cfg);

  // Processes one sequenced message; seq must be strictly increasing.
  void on_msg(const InputMsg& m);

  Emitter& emitter() { return emitter_; }
  const Emitter& emitter() const { return emitter_; }
  SessionState state() const { return state_; }
  std::uint64_t book_digest() const { return digest_; }
  std::uint64_t events_processed() const { return events_; }
  Seq last_seq() const { return last_seq_; }

  // Introspection for verification; never used on the hot path.
  const Book& book(SymbolIdx s) const { return books_[s]; }
  const Config& config() const { return cfg_; }
  const Order* find_order(OrderId id) const;
  template <typename Fn>
  void for_each_order_at(SymbolIdx s, Side sd, Price p, Fn&& fn) const {
    const Level* l = books_[s].side(sd).level_if(p);
    if (!l) return;
    for (std::uint32_t i = l->head; i != kNil; i = pool_[i].next) fn(pool_[i]);
  }

  // Deterministic snapshot; restore on a fresh Engine with the same Config.
  void save_snapshot(std::vector<std::uint8_t>& out) const;
  void load_snapshot(const std::uint8_t* data, std::size_t n);

 private:
  // Dispatch.
  void do_enter(const InputMsg& m);
  void do_cancel(const InputMsg& m);
  void do_reduce(const InputMsg& m);
  void do_replace(const InputMsg& m);
  void do_clock(const InputMsg& m);

  // Entry helpers.
  bool validate_enter(const InputMsg& m, Reason& why) const;
  void enter_continuous(const InputMsg& m);
  void enter_preopen(const InputMsg& m);
  void accept(const Order& o, Ts ts, bool display);
  void reject(Seq seq, Ts ts, Reason r);

  // Matching.
  Qty fillable(const Order& incoming) const;
  void match(std::uint32_t oi);
  bool rest(std::uint32_t oi);
  void execute(std::uint32_t resting, std::uint32_t incoming, Price px, Qty q, std::uint8_t flags);
  void cancel_order(std::uint32_t oi, Reason r);
  void remove_from_structure(std::uint32_t oi);
  std::uint32_t first_live_hidden(Book& b, Side sd, Price m_px, const Order& incoming);
  void mark_touched_sym(SymbolIdx s);

  // Post event passes.
  void run_stop_cascade();
  void run_reprice_pass();
  void reprice_symbol(SymbolIdx s);
  Price peg_price(const Order& o, const Book& b) const;

  // Auctions.
  void run_cross(SymbolIdx s, bool closing);
  void run_all_crosses(bool closing);

  // State digest bookkeeping.
  std::uint64_t order_digest(const Order& o) const;
  void digest_add(const Order& o) { digest_ ^= order_digest(o); }
  void digest_remove(const Order& o) { digest_ ^= order_digest(o); }
  void checkpoint(Ts ts);

  Config cfg_;
  SessionState state_{SessionState::halted};
  Emitter emitter_;
  OrderPool pool_;
  IdMap ids_;
  std::vector<Book> books_;
  std::vector<SymbolIdx> touched_;        // symbols with BBO movement this event
  std::vector<std::uint32_t> triggered_;  // stop cascade scratch
  std::vector<std::uint32_t> cross_buys_;
  std::vector<std::uint32_t> cross_sells_;
  // Auction candidate scan scratch: price aggregated displayed quantity.
  struct PxQty { std::int32_t px; std::int64_t qty; };
  std::vector<PxQty> cross_buy_px_;
  std::vector<PxQty> cross_sell_px_;
  std::vector<std::int32_t> cross_px_;
  std::uint64_t digest_{0};
  std::uint64_t events_{0};
  Seq last_seq_{0};
  Ts now_{0};
};

}  // namespace gavel
