#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "gavel/engine.hpp"
#include "gavel/hash.hpp"

namespace gavel::verify {

inline std::string hex64(std::uint64_t v) {
  char b[19];
  std::snprintf(b, sizeof b, "0x%016llx", static_cast<unsigned long long>(v));
  return std::string(b);
}

inline const char* state_name(OrderState s) {
  switch (s) {
    case OrderState::dead: return "dead";
    case OrderState::resting: return "resting";
    case OrderState::parked: return "parked";
    case OrderState::pending_stop: return "pending_stop";
    case OrderState::queued_market: return "queued_market";
  }
  return "?";
}

// Mirrors Engine::order_digest in src/engine.cpp exactly.
inline std::uint64_t order_digest(const Order& o) {
  const std::uint64_t pw =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.price.v)) << 32) ^
      static_cast<std::uint32_t>(o.open()) ^
      (static_cast<std::uint64_t>(o.state) << 56);
  return mix64(o.id) ^ mix64(pw);
}

// Walks the engine via its public introspection API and returns violation strings.
inline std::vector<std::string> check_invariants(const Engine& eng) {
  std::vector<std::string> out;
  auto add = [&out](std::string s) {
    if (out.size() < 64) out.push_back(std::move(s));
  };
  auto num = [](auto v) { return std::to_string(v); };
  const Config& cfg = eng.config();
  std::vector<OrderId> reach;

  for (std::uint32_t si = 0; si < cfg.num_symbols; ++si) {
    const SymbolIdx sym = static_cast<SymbolIdx>(si);
    const Book& bk = eng.book(sym);

    for (const Side sd : {Side::buy, Side::sell}) {
      const LadderSide& ls = bk.side(sd);
      const std::string where = "sym " + num(si) + (sd == Side::buy ? " bid" : " ask");
      if (ls.empty() == ls.best().valid()) add(where + ": empty() and best() disagree");
      if (ls.best().valid() && ls.level_if(ls.best()) == nullptr)
        add(where + ": best level " + num(ls.best().v) + " missing or empty");
      Price prev = kNoPrice;
      ls.for_each_level([&](Price p, const Level& l) {
        if (!p.displayable()) add(where + ": level price " + num(p.v) + " not displayable");
        if (prev.valid() && !(sd == Side::buy ? p.v < prev.v : p.v > prev.v))
          add(where + ": levels not strictly " +
              (sd == Side::buy ? std::string("descending") : std::string("ascending")) + " at " +
              num(p.v) + " after " + num(prev.v));
        prev = p;
        if (l.count == 0) add(where + ": occupied level " + num(p.v) + " has count 0");
        std::int64_t agg = 0;
        std::uint32_t cnt = 0, nonpeg = 0;
        eng.for_each_order_at(sym, sd, p, [&](const Order& o) {
          ++cnt;
          agg += o.display_open;
          const std::string oid = where + " order " + num(o.id);
          if (o.price.v != p.v) add(oid + ": price " + num(o.price.v) + " != level " + num(p.v));
          if (o.side != sd) add(oid + ": wrong side");
          if (o.symbol != sym) add(oid + ": wrong symbol");
          if (ostate(o) != OrderState::resting)
            add(oid + ": state " + state_name(ostate(o)) + " in ladder");
          if (o.display_open <= 0) add(oid + ": display_open " + num(o.display_open) + " <= 0");
          if (o.reserve < 0) add(oid + ": negative reserve " + num(o.reserve));
          if (o.cum_executed < 0) add(oid + ": negative cum_executed " + num(o.cum_executed));
          if (o.reserve > 0 && o.display_open > o.display_size)
            add(oid + ": iceberg display_open " + num(o.display_open) + " > display_size " +
                num(o.display_size));
          if (o.kind == OrderKind::market || o.kind == OrderKind::peg_mid ||
              o.kind == OrderKind::stop || o.kind == OrderKind::stop_limit)
            add(oid + ": kind not allowed in ladder");
          if (o.kind != OrderKind::peg_primary && o.kind != OrderKind::peg_market) ++nonpeg;
          if (eng.find_order(o.id) != &o) add(oid + ": id map does not resolve to this order");
          reach.push_back(o.id);
        });
        if (agg != l.agg_qty)
          add(where + " level " + num(p.v) + ": agg_qty " + num(l.agg_qty) + " != walked sum " +
              num(agg));
        if (cnt != l.count)
          add(where + " level " + num(p.v) + ": count " + num(l.count) + " != walked count " +
              num(cnt));
        if (nonpeg != l.nonpeg_count)
          add(where + " level " + num(p.v) + ": nonpeg_count " + num(l.nonpeg_count) +
              " != walked " + num(nonpeg));
      });
    }

    if (eng.state() == SessionState::open) {
      const Price bb = bk.bid.best(), ba = bk.ask.best();
      if (bb.valid() && ba.valid() && bb.v >= ba.v)
        add("sym " + num(si) + ": crossed book in open state, bid " + num(bb.v) + " ask " +
            num(ba.v));
    }

    // Auxiliary id lists: every resolvable id must match the list's expected shape.
    auto check_list = [&](const std::vector<OrderId>& v, const char* lname, auto&& ok_fn) {
      for (const OrderId id : v) {
        if (id == 0) continue;
        const Order* o = eng.find_order(id);
        if (!o) continue;
        std::string why;
        if (!ok_fn(*o, why))
          add("sym " + num(si) + " " + lname + " order " + num(id) + ": " + why);
        reach.push_back(id);
      }
    };
    for (int k = 0; k < 2; ++k) {
      const Side sd = k == 0 ? Side::buy : Side::sell;
      check_list(bk.hidden_mids[k], k ? "hidden_mids[sell]" : "hidden_mids[buy]",
                 [&](const Order& o, std::string& why) {
                   if (o.kind != OrderKind::peg_mid) { why = "kind not peg_mid"; return false; }
                   if (ostate(o) != OrderState::resting) {
                     why = std::string("state ") + state_name(ostate(o)); return false;
                   }
                   if (o.side != sd) { why = "wrong side"; return false; }
                   if (o.symbol != sym) { why = "wrong symbol"; return false; }
                   if (o.open() <= 0) { why = "nonpositive open"; return false; }
                   return true;
                 });
      check_list(bk.stops[k], k ? "stops[sell]" : "stops[buy]",
                 [&](const Order& o, std::string& why) {
                   if (ostate(o) != OrderState::pending_stop) {
                     why = std::string("state ") + state_name(ostate(o)); return false;
                   }
                   if (o.kind != OrderKind::stop && o.kind != OrderKind::stop_limit) {
                     why = "kind not stop"; return false;
                   }
                   if (o.side != sd) { why = "wrong side"; return false; }
                   if (o.symbol != sym) { why = "wrong symbol"; return false; }
                   if (!o.trigger.displayable()) { why = "bad trigger price"; return false; }
                   if (o.open() <= 0) { why = "nonpositive open"; return false; }
                   return true;
                 });
      check_list(bk.market_queue[k], k ? "market_queue[sell]" : "market_queue[buy]",
                 [&](const Order& o, std::string& why) {
                   if (ostate(o) != OrderState::queued_market) {
                     why = std::string("state ") + state_name(ostate(o)); return false;
                   }
                   if (o.kind != OrderKind::market) { why = "kind not market"; return false; }
                   if (o.side != sd) { why = "wrong side"; return false; }
                   if (o.symbol != sym) { why = "wrong symbol"; return false; }
                   if (o.open() <= 0) { why = "nonpositive open"; return false; }
                   return true;
                 });
    }
    check_list(bk.pegs, "pegs", [&](const Order& o, std::string& why) {
      if (o.kind != OrderKind::peg_primary && o.kind != OrderKind::peg_market) {
        why = "kind not a ladder peg"; return false;
      }
      if (ostate(o) != OrderState::resting && ostate(o) != OrderState::parked) {
        why = std::string("state ") + state_name(ostate(o)); return false;
      }
      if (ostate(o) == OrderState::parked && o.price.valid()) {
        why = "parked with a price"; return false;
      }
      if (ostate(o) == OrderState::resting && !o.price.displayable()) {
        why = "resting without displayable price"; return false;
      }
      if (o.symbol != sym) { why = "wrong symbol"; return false; }
      if (o.open() <= 0) { why = "nonpositive open"; return false; }
      return true;
    });
  }

  // Digest: every live order is reachable from the books; recompute and compare.
  std::sort(reach.begin(), reach.end());
  reach.erase(std::unique(reach.begin(), reach.end()), reach.end());
  std::uint64_t digest = 0;
  for (const OrderId id : reach) {
    const Order* o = eng.find_order(id);
    if (o) digest ^= order_digest(*o);
  }
  if (digest != eng.book_digest())
    add("digest mismatch: walked " + hex64(digest) + " engine " + hex64(eng.book_digest()) +
        " over " + std::to_string(reach.size()) + " reachable orders");
  return out;
}

}  // namespace gavel::verify
