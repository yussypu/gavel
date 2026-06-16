#pragma once
#include <cstdint>
#include <unordered_map>
#include "gavel/input.hpp"
#include "gavel/itch/itch.hpp"

// Mapping from one symbol's ITCH flow onto a gavel input stream plus exec sidecar.
namespace gavel::itch {

// At most two synthesized clocks plus one mapped record per ITCH message.
struct MapOut {
  std::uint32_t nmsgs;
  bool has_exec;
  InputMsg msgs[3];
  ExecRecord exec;
};

struct MapCounters {
  std::uint64_t by_type[256]{};
  std::uint64_t sub_penny{0};
  std::uint64_t orphan_ref{0};
};

class SymbolMapper {
 public:
  MapCounters counters;

  void map(const Message& m, MapOut& out) {
    out.nmsgs = 0;
    out.has_exec = false;
    ++counters.by_type[static_cast<unsigned char>(m.type)];
    switch (m.type) {
      case 'A': case 'F': on_add(m, out); break;
      case 'E': on_exec(m, out); break;
      case 'C': on_exec_price(m, out); break;
      case 'X': on_cancel(m, out); break;
      case 'D': on_delete(m, out); break;
      case 'U': on_replace(m, out); break;
      default: break;
    }
  }

  Seq last_seq() const { return seq_; }
  std::size_t live_refs() const { return refs_.size(); }

 private:
  struct Ref {
    OrderId id;
    std::int32_t price;
    std::int64_t open;
  };

  // The whole tape replays as continuous trading, so clocks are synthesized once up front.
  void ensure_started(Ts ts, MapOut& out) {
    if (started_) return;
    started_ = true;
    out.msgs[out.nmsgs++] = make_clock(++seq_, ts, ClockAction::session_start);
    out.msgs[out.nmsgs++] = make_clock(++seq_, ts, ClockAction::open_cross);
  }

  void on_add(const Message& m, MapOut& out) {
    if (!whole_cent(m.add.price)) { ++counters.sub_penny; return; }
    ensure_started(m.h.ts, out);
    const Side side = m.add.side == 'B' ? Side::buy : Side::sell;
    const auto qty = static_cast<Qty>(m.add.shares);
    const std::int32_t px = to_half_ticks(m.add.price);
    out.msgs[out.nmsgs++] = make_enter(++seq_, m.h.ts, 0, side, OrderKind::limit, Tif::day, qty, px);
    refs_[m.add.ref] = Ref{seq_, px, qty};
  }

  void on_exec(const Message& m, MapOut& out) {
    auto it = refs_.find(m.exec.ref);
    if (it == refs_.end()) { ++counters.orphan_ref; return; }
    fill_exec(out, it->second.id, m.exec.ref, m.h.ts, m.exec.shares, it->second.price, kExecKindE, 0);
    consume(it, m.exec.shares);
  }

  void on_exec_price(const Message& m, MapOut& out) {
    auto it = refs_.find(m.exec.ref);
    if (it == refs_.end()) { ++counters.orphan_ref; return; }
    const std::uint8_t flags = whole_cent(m.exec.price) ? std::uint8_t{0} : kExecFlagSubPenny;
    fill_exec(out, it->second.id, m.exec.ref, m.h.ts, m.exec.shares, to_half_ticks(m.exec.price), kExecKindC, flags);
    consume(it, m.exec.shares);
  }

  void on_cancel(const Message& m, MapOut& out) {
    auto it = refs_.find(m.cancel.ref);
    if (it == refs_.end()) { ++counters.orphan_ref; return; }
    ensure_started(m.h.ts, out);
    out.msgs[out.nmsgs++] = make_reduce(++seq_, m.h.ts, it->second.id, static_cast<Qty>(m.cancel.shares));
    consume(it, m.cancel.shares);
  }

  void on_delete(const Message& m, MapOut& out) {
    auto it = refs_.find(m.del.ref);
    if (it == refs_.end()) { ++counters.orphan_ref; return; }
    ensure_started(m.h.ts, out);
    out.msgs[out.nmsgs++] = make_cancel(++seq_, m.h.ts, it->second.id);
    refs_.erase(it);
  }

  void on_replace(const Message& m, MapOut& out) {
    auto it = refs_.find(m.replace.orig);
    if (it == refs_.end()) { ++counters.orphan_ref; return; }
    ensure_started(m.h.ts, out);
    if (!whole_cent(m.replace.price)) {
      // A sub penny replace price cannot be represented; cancel the old order instead.
      ++counters.sub_penny;
      out.msgs[out.nmsgs++] = make_cancel(++seq_, m.h.ts, it->second.id);
      refs_.erase(it);
      return;
    }
    const std::int32_t px = to_half_ticks(m.replace.price);
    const auto qty = static_cast<Qty>(m.replace.shares);
    out.msgs[out.nmsgs++] = make_replace(++seq_, m.h.ts, it->second.id, qty, px);
    refs_.erase(it);
    refs_[m.replace.fresh] = Ref{seq_, px, qty};
  }

  void fill_exec(MapOut& out, OrderId id, std::uint64_t ref, Ts ts, std::uint32_t shares,
                 std::int32_t px, std::uint8_t kind, std::uint8_t flags) {
    out.has_exec = true;
    out.exec = ExecRecord{};
    out.exec.gavel_id = id;
    out.exec.itch_ref = ref;
    out.exec.ts = ts;
    out.exec.after_seq = seq_;
    out.exec.qty = static_cast<Qty>(shares);
    out.exec.price = px;
    out.exec.kind = kind;
    out.exec.flags = flags;
  }

  void consume(std::unordered_map<std::uint64_t, Ref>::iterator it, std::uint32_t shares) {
    it->second.open -= static_cast<std::int64_t>(shares);
    if (it->second.open <= 0) refs_.erase(it);
  }

  Seq seq_{0};
  bool started_{false};
  std::unordered_map<std::uint64_t, Ref> refs_;
};

}  // namespace gavel::itch
