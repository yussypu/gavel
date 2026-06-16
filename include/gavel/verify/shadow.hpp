#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "gavel/output.hpp"

namespace gavel::verify {

// Rebuilds order state purely from output bytes; downstream views need no engine access.
class ShadowView {
 public:
  void consume(const std::uint8_t* data, std::size_t n) {
    pend_.insert(pend_.end(), data, data + n);
    std::size_t off = 0;
    while (pend_.size() - off >= 2) {
      const std::size_t sz = pend_[off + 1];
      if (pend_.size() - off < sz + 2) break;
      const std::uint64_t pre_hash = hash_;
      hash_ = fnv1a64(pend_.data() + off, sz + 2, hash_);
      ++events_;
      handle(static_cast<EventType>(pend_[off]), pend_.data() + off + 2, sz, pre_hash);
      off += sz + 2;
    }
    pend_.erase(pend_.begin(), pend_.begin() + static_cast<std::ptrdiff_t>(off));
  }

  const std::vector<std::string>& violations() const { return v_; }
  std::size_t live_count() const { return open_.size(); }
  std::size_t max_live() const { return max_live_; }
  std::uint64_t events_seen() const { return events_; }
  std::uint64_t stream_hash() const { return hash_; }

 private:
  static std::string hex(std::uint64_t x) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%016llx", static_cast<unsigned long long>(x));
    return std::string(b);
  }

  void flag(std::string s) {
    if (v_.size() < 64) v_.push_back("event " + std::to_string(events_) + ": " + std::move(s));
  }

  template <typename Ev>
  bool get(Ev& ev, const std::uint8_t* p, std::size_t sz, const char* name) {
    if (sz != sizeof(Ev)) {
      flag(std::string(name) + " payload size " + std::to_string(sz) + " != " +
           std::to_string(sizeof(Ev)));
      return false;
    }
    std::memcpy(&ev, p, sizeof(Ev));
    return true;
  }

  void take(OrderId id, Qty q, bool resting, Qty remaining) {
    auto it = open_.find(id);
    if (it == open_.end()) {
      flag(std::string("Executed names unknown or dead ") + (resting ? "resting" : "incoming") +
           " id " + std::to_string(id));
      return;
    }
    if (it->second < q) {
      flag("Executed qty " + std::to_string(q) + " exceeds known open " +
           std::to_string(it->second) + " for id " + std::to_string(id));
      open_.erase(it);
      return;
    }
    it->second -= q;
    if (resting && it->second != static_cast<std::int64_t>(remaining))
      flag("resting_remaining " + std::to_string(remaining) + " != tracked " +
           std::to_string(it->second) + " for id " + std::to_string(id));
    if (it->second == 0) open_.erase(it);
  }

  void handle(EventType t, const std::uint8_t* p, std::size_t sz, std::uint64_t pre_hash) {
    switch (t) {
      case EventType::accepted: {
        EvAccepted e;
        if (!get(e, p, sz, "Accepted")) return;
        if (open_.count(e.id)) {
          flag("duplicate Accepted id " + std::to_string(e.id));
          return;
        }
        if (e.qty <= 0) {
          flag("Accepted nonpositive qty for id " + std::to_string(e.id));
          return;
        }
        open_[e.id] = e.qty;
        if (open_.size() > max_live_) max_live_ = open_.size();
        return;
      }
      case EventType::rejected: {
        EvRejected e;
        get(e, p, sz, "Rejected");
        return;
      }
      case EventType::canceled: {
        EvCanceled e;
        if (!get(e, p, sz, "Canceled")) return;
        auto it = open_.find(e.id);
        if (it == open_.end()) {
          flag("Canceled unknown id " + std::to_string(e.id));
          return;
        }
        if (it->second != e.qty_canceled)
          flag("Canceled qty " + std::to_string(e.qty_canceled) + " != tracked open " +
               std::to_string(it->second) + " for id " + std::to_string(e.id));
        open_.erase(it);
        return;
      }
      case EventType::reduced: {
        EvReduced e;
        if (!get(e, p, sz, "Reduced")) return;
        auto it = open_.find(e.id);
        if (it == open_.end()) {
          flag("Reduced unknown id " + std::to_string(e.id));
          return;
        }
        it->second -= e.qty_reduced;
        if (it->second <= 0) {
          flag("Reduced to nonpositive remaining " + std::to_string(it->second) + " for id " +
               std::to_string(e.id));
          open_.erase(it);
          return;
        }
        if (it->second != e.remaining)
          flag("Reduced remaining " + std::to_string(e.remaining) + " != tracked " +
               std::to_string(it->second) + " for id " + std::to_string(e.id));
        return;
      }
      case EventType::replaced: {
        EvReplaced e;
        if (!get(e, p, sz, "Replaced")) return;
        if (!open_.count(e.old_id)) {
          flag("Replaced unknown old id " + std::to_string(e.old_id));
          return;
        }
        open_.erase(e.old_id);
        return;
      }
      case EventType::executed: {
        EvExecuted e;
        if (!get(e, p, sz, "Executed")) return;
        if (e.qty <= 0) {
          flag("Executed nonpositive qty");
          return;
        }
        if (e.resting == e.incoming) {
          flag("Executed with resting == incoming id " + std::to_string(e.resting));
          return;
        }
        take(e.resting, e.qty, true, e.resting_remaining);
        take(e.incoming, e.qty, false, 0);
        return;
      }
      case EventType::refilled: {
        EvRefilled e;
        if (!get(e, p, sz, "Refilled")) return;
        if (!open_.count(e.id)) flag("Refilled unknown id " + std::to_string(e.id));
        if (e.display_qty <= 0) flag("Refilled nonpositive display qty");
        return;
      }
      case EventType::repriced: {
        EvRepriced e;
        if (!get(e, p, sz, "Repriced")) return;
        if (!open_.count(e.id)) flag("Repriced unknown id " + std::to_string(e.id));
        return;
      }
      case EventType::stop_triggered: {
        EvStopTriggered e;
        if (!get(e, p, sz, "StopTriggered")) return;
        if (!open_.count(e.id)) flag("StopTriggered unknown id " + std::to_string(e.id));
        return;
      }
      case EventType::auction_result: {
        EvAuctionResult e;
        get(e, p, sz, "AuctionResult");
        return;
      }
      case EventType::session_event: {
        EvSessionEvent e;
        get(e, p, sz, "SessionEvent");
        return;
      }
      case EventType::state_hash: {
        EvStateHash e;
        if (!get(e, p, sz, "StateHash")) return;
        if (e.out_hash != pre_hash)
          flag("StateHash out_hash " + hex(e.out_hash) + " != consumed stream hash " +
               hex(pre_hash));
        return;
      }
    }
    flag("unknown event type " + std::to_string(static_cast<unsigned>(t)));
  }

  std::vector<std::uint8_t> pend_;
  std::unordered_map<OrderId, std::int64_t> open_;
  std::vector<std::string> v_;
  std::uint64_t hash_{kFnvOffset};
  std::uint64_t events_{0};
  std::size_t max_live_{0};
};

}  // namespace gavel::verify
