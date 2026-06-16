#include "doctest.h"
#include <cstdio>
#include <vector>
#include "gavel/engine.hpp"
#include "gavel/itch/extract.hpp"

using namespace gavel;
using namespace gavel::itch;

namespace {

struct Buf {
  std::vector<std::uint8_t> v;
  void u8(std::uint8_t x) { v.push_back(x); }
  void u16(std::uint16_t x) { u8(static_cast<std::uint8_t>(x >> 8)); u8(static_cast<std::uint8_t>(x & 0xff)); }
  void u32(std::uint32_t x) { u16(static_cast<std::uint16_t>(x >> 16)); u16(static_cast<std::uint16_t>(x & 0xffff)); }
  void u48(std::uint64_t x) { u16(static_cast<std::uint16_t>(x >> 32)); u32(static_cast<std::uint32_t>(x & 0xffffffff)); }
  void u64(std::uint64_t x) { u32(static_cast<std::uint32_t>(x >> 32)); u32(static_cast<std::uint32_t>(x & 0xffffffff)); }
  void ch(char c) { u8(static_cast<std::uint8_t>(c)); }
  void stock(const char* s) {
    std::size_t i = 0;
    for (; s[i] && i < 8; ++i) ch(s[i]);
    for (; i < 8; ++i) ch(' ');
  }
};

struct Tape {
  std::vector<std::uint8_t> bytes;
  void frame(const Buf& b) {
    bytes.push_back(static_cast<std::uint8_t>(b.v.size() >> 8));
    bytes.push_back(static_cast<std::uint8_t>(b.v.size() & 0xff));
    bytes.insert(bytes.end(), b.v.begin(), b.v.end());
  }
  std::FILE* file() const {
    std::FILE* f = std::tmpfile();
    REQUIRE(f != nullptr);
    if (!bytes.empty()) REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::rewind(f);
    return f;
  }
};

Buf hdr(char type, std::uint16_t locate, std::uint64_t ts) {
  Buf b;
  b.ch(type);
  b.u16(locate);
  b.u16(7);
  b.u48(ts);
  return b;
}

Buf m_sys(std::uint64_t ts, char code) {
  Buf b = hdr('S', 0, ts);
  b.ch(code);
  return b;
}

Buf m_dir(std::uint16_t loc, std::uint64_t ts, const char* st, std::uint32_t lot) {
  Buf b = hdr('R', loc, ts);
  b.stock(st);
  b.ch('Q'); b.ch('N');
  b.u32(lot);
  b.ch('N'); b.ch('C'); b.ch('Z'); b.ch(' ');
  b.ch('P'); b.ch('N'); b.ch('N'); b.ch('1'); b.ch('N');
  b.u32(0);
  b.ch('N');
  return b;
}

Buf m_act(std::uint16_t loc, std::uint64_t ts, const char* st, char state) {
  Buf b = hdr('H', loc, ts);
  b.stock(st);
  b.ch(state);
  b.ch(' ');
  b.u32(0);
  return b;
}

Buf m_add(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t sh,
          const char* st, std::uint32_t px, const char* mpid = nullptr) {
  Buf b = hdr(mpid ? 'F' : 'A', loc, ts);
  b.u64(ref);
  b.ch(side);
  b.u32(sh);
  b.stock(st);
  b.u32(px);
  if (mpid) for (int i = 0; i < 4; ++i) b.ch(mpid[i]);
  return b;
}

Buf m_exec(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t sh, std::uint64_t match) {
  Buf b = hdr('E', loc, ts);
  b.u64(ref);
  b.u32(sh);
  b.u64(match);
  return b;
}

Buf m_exec_px(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t sh,
              std::uint64_t match, char printable, std::uint32_t px) {
  Buf b = m_exec(loc, ts, ref, sh, match);
  b.v[0] = static_cast<std::uint8_t>('C');
  b.ch(printable);
  b.u32(px);
  return b;
}

Buf m_cancel(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t sh) {
  Buf b = hdr('X', loc, ts);
  b.u64(ref);
  b.u32(sh);
  return b;
}

Buf m_delete(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) {
  Buf b = hdr('D', loc, ts);
  b.u64(ref);
  return b;
}

Buf m_replace(std::uint16_t loc, std::uint64_t ts, std::uint64_t orig, std::uint64_t fresh,
              std::uint32_t sh, std::uint32_t px) {
  Buf b = hdr('U', loc, ts);
  b.u64(orig);
  b.u64(fresh);
  b.u32(sh);
  b.u32(px);
  return b;
}

Buf m_trade(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t sh,
            const char* st, std::uint32_t px, std::uint64_t match) {
  Buf b = hdr('P', loc, ts);
  b.u64(ref);
  b.ch(side);
  b.u32(sh);
  b.stock(st);
  b.u32(px);
  b.u64(match);
  return b;
}

Buf m_cross(std::uint16_t loc, std::uint64_t ts, std::uint64_t sh, const char* st,
            std::uint32_t px, std::uint64_t match, char type) {
  Buf b = hdr('Q', loc, ts);
  b.u64(sh);
  b.stock(st);
  b.u32(px);
  b.u64(match);
  b.ch(type);
  return b;
}

Buf m_broken(std::uint16_t loc, std::uint64_t ts, std::uint64_t match) {
  Buf b = hdr('B', loc, ts);
  b.u64(match);
  return b;
}

Message must_parse(const Buf& b) {
  Message m{};
  REQUIRE(parse(b.v.data(), b.v.size(), m) == Parse::ok);
  return m;
}

}  // namespace

TEST_CASE("big endian readers") {
  const std::uint8_t p[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  CHECK(be16(p) == 0x0123);
  CHECK(be32(p) == 0x01234567u);
  CHECK(be48(p) == 0x0123456789abull);
  CHECK(be64(p) == 0x0123456789abcdefull);
}

TEST_CASE("price conversion and sub penny detection") {
  CHECK(whole_cent(1234500));
  CHECK_FALSE(whole_cent(1234550));
  CHECK_FALSE(whole_cent(1234567));
  // $123.45 is 1234500 itch units = 24690 half ticks, even, displayable.
  CHECK(to_half_ticks(1234500) == 24690);
  CHECK(Price{to_half_ticks(1234500)}.displayable());
  CHECK(to_half_ticks(100) == 2);
}

TEST_CASE("parses every supported type with big endian fields") {
  const std::uint64_t ts = 0x123456789abull;
  const Message s = must_parse(m_sys(ts, 'O'));
  CHECK(s.type == 'S');
  CHECK(s.h.ts == ts);
  CHECK(s.sys.code == 'O');

  const Message r = must_parse(m_dir(77, ts, "AAPL", 100));
  CHECK(r.h.locate == 77);
  CHECK(r.h.tracking == 7);
  CHECK(trim_stock(r.dir.stock) == "AAPL");
  CHECK(r.dir.round_lot == 100);

  const Message h = must_parse(m_act(77, ts, "AAPL", 'T'));
  CHECK(trim_stock(h.action.stock) == "AAPL");
  CHECK(h.action.state == 'T');

  const Message a = must_parse(m_add(77, ts, 0xdeadbeefcafeull, 'B', 300, "AAPL", 1234500));
  CHECK(a.type == 'A');
  CHECK(a.add.ref == 0xdeadbeefcafeull);
  CHECK(a.add.side == 'B');
  CHECK(a.add.shares == 300);
  CHECK(trim_stock(a.add.stock) == "AAPL");
  CHECK(a.add.price == 1234500);
  CHECK_FALSE(a.add.has_mpid);

  const Message f = must_parse(m_add(77, ts, 9, 'S', 200, "AAPL", 1234600, "JPMS"));
  CHECK(f.type == 'F');
  CHECK(f.add.has_mpid);
  CHECK(std::string(f.add.mpid) == "JPMS");

  const Message e = must_parse(m_exec(77, ts, 9, 50, 1111));
  CHECK(e.exec.ref == 9);
  CHECK(e.exec.shares == 50);
  CHECK(e.exec.match == 1111);
  CHECK_FALSE(e.exec.has_price);

  const Message c = must_parse(m_exec_px(77, ts, 9, 25, 1112, 'Y', 1234550));
  CHECK(c.exec.has_price);
  CHECK(c.exec.printable == 'Y');
  CHECK(c.exec.price == 1234550);

  const Message x = must_parse(m_cancel(77, ts, 9, 10));
  CHECK(x.cancel.ref == 9);
  CHECK(x.cancel.shares == 10);

  const Message d = must_parse(m_delete(77, ts, 9));
  CHECK(d.del.ref == 9);

  const Message u = must_parse(m_replace(77, ts, 9, 10, 400, 1230000));
  CHECK(u.replace.orig == 9);
  CHECK(u.replace.fresh == 10);
  CHECK(u.replace.shares == 400);
  CHECK(u.replace.price == 1230000);

  const Message p = must_parse(m_trade(77, ts, 0, 'B', 75, "AAPL", 1234500, 2222));
  CHECK(p.trade.shares == 75);
  CHECK(p.trade.match == 2222);

  const Message q = must_parse(m_cross(77, ts, 500000, "AAPL", 1234500, 3333, 'O'));
  CHECK(q.cross.shares == 500000);
  CHECK(q.cross.price == 1234500);
  CHECK(q.cross.cross_type == 'O');

  const Message b = must_parse(m_broken(77, ts, 3333));
  CHECK(b.broken.match == 3333);
}

TEST_CASE("reader frames a tape and skips unknown types by length") {
  Tape t;
  t.frame(m_add(1, 10, 5, 'B', 100, "MSFT", 4000000));
  Buf small_unknown;
  small_unknown.ch('Z');
  for (int i = 0; i < 6; ++i) small_unknown.u8(0xee);
  t.frame(small_unknown);
  Buf big_unknown;
  big_unknown.ch('I');
  for (int i = 0; i < 300; ++i) big_unknown.u8(0xdd);
  t.frame(big_unknown);
  t.frame(m_delete(1, 11, 5));

  std::FILE* f = t.file();
  Reader rd(f);
  Message m{};
  CHECK(rd.next(m) == Reader::Status::msg);
  CHECK(m.type == 'A');
  CHECK(rd.next(m) == Reader::Status::unknown);
  CHECK(rd.next(m) == Reader::Status::unknown);
  CHECK(rd.next(m) == Reader::Status::msg);
  CHECK(m.type == 'D');
  CHECK(rd.next(m) == Reader::Status::eof);
  CHECK(rd.messages() == 2);
  CHECK(rd.unknown() == 2);
  std::fclose(f);
}

TEST_CASE("reader reports truncation and bad lengths") {
  Tape t;
  t.frame(m_sys(1, 'O'));
  Buf cut = m_add(1, 10, 5, 'B', 100, "MSFT", 4000000);
  t.frame(cut);
  t.bytes.erase(t.bytes.end() - 4, t.bytes.end());
  std::FILE* f = t.file();
  Reader rd(f);
  Message m{};
  CHECK(rd.next(m) == Reader::Status::msg);
  CHECK(rd.next(m) == Reader::Status::truncated);
  std::fclose(f);

  // A known type framed with the wrong length is consumed and flagged.
  Tape t2;
  Buf bad = m_delete(1, 11, 5);
  bad.u8(0);
  t2.frame(bad);
  t2.frame(m_sys(2, 'C'));
  std::FILE* f2 = t2.file();
  Reader rd2(f2);
  CHECK(rd2.next(m) == Reader::Status::bad_msg);
  CHECK(rd2.next(m) == Reader::Status::msg);
  CHECK(m.type == 'S');
  std::fclose(f2);
}

TEST_CASE("mapper synthesizes clocks then enters") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 42, 'B', 300, "AAPL", 1234500)), o);
  REQUIRE(o.nmsgs == 3);
  CHECK(o.msgs[0].type == MsgType::clock);
  CHECK(o.msgs[0].seq == 1);
  CHECK(o.msgs[0].clock_action == static_cast<std::uint8_t>(ClockAction::session_start));
  CHECK(o.msgs[1].seq == 2);
  CHECK(o.msgs[1].clock_action == static_cast<std::uint8_t>(ClockAction::open_cross));
  CHECK(o.msgs[2].type == MsgType::enter);
  CHECK(o.msgs[2].seq == 3);
  CHECK(o.msgs[2].ts == 1000);
  CHECK(o.msgs[2].side == Side::buy);
  CHECK(o.msgs[2].kind == OrderKind::limit);
  CHECK(o.msgs[2].tif == Tif::day);
  CHECK(o.msgs[2].qty == 300);
  CHECK(o.msgs[2].price == 24690);
  CHECK(o.msgs[2].aux == 0);
  CHECK_FALSE(o.has_exec);

  sm.map(must_parse(m_add(1, 1001, 43, 'S', 100, "AAPL", 1234600, "JPMS")), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].seq == 4);
  CHECK(o.msgs[0].side == Side::sell);
  CHECK(sm.live_refs() == 2);
}

TEST_CASE("mapper sub penny add is counted and skipped") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 42, 'B', 300, "AAPL", 1234567)), o);
  CHECK(o.nmsgs == 0);
  CHECK(sm.counters.sub_penny == 1);
  CHECK(sm.live_refs() == 0);
  // A delete for the skipped ref is an orphan, not a record.
  sm.map(must_parse(m_delete(1, 1001, 42)), o);
  CHECK(o.nmsgs == 0);
  CHECK(sm.counters.orphan_ref == 1);
}

TEST_CASE("mapper maps E and C to the exec sidecar") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 42, 'B', 300, "AAPL", 1234500)), o);
  sm.map(must_parse(m_exec(1, 1001, 42, 100, 9001)), o);
  CHECK(o.nmsgs == 0);
  REQUIRE(o.has_exec);
  CHECK(o.exec.gavel_id == 3);
  CHECK(o.exec.itch_ref == 42);
  CHECK(o.exec.ts == 1001);
  CHECK(o.exec.after_seq == 3);
  CHECK(o.exec.qty == 100);
  CHECK(o.exec.price == 24690);
  CHECK(o.exec.kind == kExecKindE);
  CHECK(o.exec.flags == 0);

  sm.map(must_parse(m_exec_px(1, 1002, 42, 50, 9002, 'Y', 1234550)), o);
  REQUIRE(o.has_exec);
  CHECK(o.exec.kind == kExecKindC);
  CHECK(o.exec.flags == kExecFlagSubPenny);
  CHECK(o.exec.price == 24691);

  // 100 + 50 executed leaves 150 open; executing the rest releases the ref.
  sm.map(must_parse(m_exec(1, 1003, 42, 150, 9003)), o);
  CHECK(sm.live_refs() == 0);
  sm.map(must_parse(m_exec(1, 1004, 42, 10, 9004)), o);
  CHECK_FALSE(o.has_exec);
  CHECK(sm.counters.orphan_ref == 1);
}

TEST_CASE("mapper maps X to reduce and D to cancel") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 42, 'S', 300, "AAPL", 1234500)), o);
  sm.map(must_parse(m_cancel(1, 1001, 42, 100)), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].type == MsgType::reduce);
  CHECK(o.msgs[0].seq == 4);
  CHECK(o.msgs[0].target == 3);
  CHECK(o.msgs[0].qty == 100);
  sm.map(must_parse(m_delete(1, 1002, 42)), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].type == MsgType::cancel);
  CHECK(o.msgs[0].seq == 5);
  CHECK(o.msgs[0].target == 3);
  CHECK(sm.live_refs() == 0);
  // X reducing to zero releases the ref, so a later E is an orphan.
  sm.map(must_parse(m_add(1, 1003, 50, 'S', 100, "AAPL", 1234500)), o);
  sm.map(must_parse(m_cancel(1, 1004, 50, 100)), o);
  CHECK(sm.live_refs() == 0);
  sm.map(must_parse(m_exec(1, 1005, 50, 10, 9000)), o);
  CHECK(sm.counters.orphan_ref == 1);
}

TEST_CASE("mapper replace chain keeps ref to id mapping") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 10, 'B', 300, "AAPL", 1234500)), o);
  sm.map(must_parse(m_replace(1, 1001, 10, 11, 200, 1234600)), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].type == MsgType::replace);
  CHECK(o.msgs[0].seq == 4);
  CHECK(o.msgs[0].target == 3);
  CHECK(o.msgs[0].qty == 200);
  CHECK(o.msgs[0].price == 24692);
  sm.map(must_parse(m_replace(1, 1002, 11, 12, 150, 1234700)), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].seq == 5);
  CHECK(o.msgs[0].target == 4);
  // The exec on the final ref points at the latest gavel id.
  sm.map(must_parse(m_exec(1, 1003, 12, 50, 9000)), o);
  REQUIRE(o.has_exec);
  CHECK(o.exec.gavel_id == 5);
  CHECK(o.exec.price == 24694);
  // The replaced away refs are gone.
  sm.map(must_parse(m_exec(1, 1004, 10, 5, 9001)), o);
  CHECK(sm.counters.orphan_ref == 1);
}

TEST_CASE("mapper sub penny replace cancels the old order") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_add(1, 1000, 10, 'B', 300, "AAPL", 1234500)), o);
  sm.map(must_parse(m_replace(1, 1001, 10, 11, 200, 1234567)), o);
  REQUIRE(o.nmsgs == 1);
  CHECK(o.msgs[0].type == MsgType::cancel);
  CHECK(o.msgs[0].target == 3);
  CHECK(sm.counters.sub_penny == 1);
  CHECK(sm.live_refs() == 0);
}

TEST_CASE("mapper counts P Q B without emitting records") {
  SymbolMapper sm;
  MapOut o;
  sm.map(must_parse(m_trade(1, 1000, 0, 'B', 75, "AAPL", 1234500, 1)), o);
  CHECK(o.nmsgs == 0);
  sm.map(must_parse(m_cross(1, 1001, 1000, "AAPL", 1234500, 2, 'O')), o);
  CHECK(o.nmsgs == 0);
  sm.map(must_parse(m_broken(1, 1002, 2)), o);
  CHECK(o.nmsgs == 0);
  CHECK(sm.counters.by_type[static_cast<unsigned char>('P')] == 1);
  CHECK(sm.counters.by_type[static_cast<unsigned char>('Q')] == 1);
  CHECK(sm.counters.by_type[static_cast<unsigned char>('B')] == 1);
}

TEST_CASE("mapped records replay cleanly and execute under a counterfactual aggressor") {
  SymbolMapper sm;
  Engine eng(Config{1, 8192, 1 << 12, 1u << 26, 0});
  MapOut o;
  auto run = [&](const Buf& b) {
    sm.map(must_parse(b), o);
    for (std::uint32_t i = 0; i < o.nmsgs; ++i) eng.on_msg(o.msgs[i]);
  };
  run(m_add(1, 1000, 42, 'B', 300, "AAPL", 1234500));
  run(m_add(1, 1001, 43, 'B', 200, "AAPL", 1234500));
  const Order* resting = eng.find_order(3);
  REQUIRE(resting != nullptr);
  CHECK(resting->price == Price{24690});
  CHECK(eng.book(0).bid.best() == Price{24690});
  // Counterfactual IOC sell for 100 must hit order 3, the front of the level.
  eng.emitter().drain();
  eng.on_msg(make_enter(100, 2000, 0, Side::sell, OrderKind::limit, Tif::ioc, 100, 24690));
  const auto& buf = eng.emitter().buffer();
  std::size_t i = 0, nexec = 0;
  while (i + 2 <= buf.size()) {
    if (static_cast<EventType>(buf[i]) == EventType::executed) {
      EvExecuted e;
      std::memcpy(&e, buf.data() + i + 2, sizeof(e));
      CHECK(e.resting == 3);
      CHECK(e.qty == 100);
      ++nexec;
    }
    i += 2 + buf[i + 1];
  }
  CHECK(nexec == 1);
}
