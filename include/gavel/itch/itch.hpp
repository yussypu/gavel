#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "gavel/types.hpp"

// NASDAQ TotalView ITCH 5.0 parsing over length prefixed files, streaming only.
namespace gavel::itch {

inline std::uint16_t be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) {
  return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) {
  return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

// ITCH prices carry 4 implied decimals; one gavel half tick is half a cent.
inline constexpr std::uint32_t kHalfTickUnits = 50;
constexpr bool whole_cent(std::uint32_t px) { return px % 100u == 0; }
constexpr std::int32_t to_half_ticks(std::uint32_t px) { return static_cast<std::int32_t>(px / kHalfTickUnits); }

inline std::string trim_stock(const char* s) {
  std::size_t n = 0;
  while (s[n]) ++n;
  while (n > 0 && s[n - 1] == ' ') --n;
  return std::string(s, n);
}

struct Header {
  std::uint16_t locate;
  std::uint16_t tracking;
  std::uint64_t ts;  // nanoseconds since midnight
};

struct SystemEvent { char code; };
struct StockDirectory { std::uint32_t round_lot; char stock[9]; };
struct TradingAction { char stock[9]; char state; };
struct AddOrder { std::uint64_t ref; std::uint32_t shares; std::uint32_t price; char stock[9]; char side; char mpid[5]; bool has_mpid; };
struct OrderExecuted { std::uint64_t ref; std::uint64_t match; std::uint32_t shares; std::uint32_t price; char printable; bool has_price; };
struct OrderCancel { std::uint64_t ref; std::uint32_t shares; };
struct OrderDelete { std::uint64_t ref; };
struct OrderReplace { std::uint64_t orig; std::uint64_t fresh; std::uint32_t shares; std::uint32_t price; };
struct Trade { std::uint64_t ref; std::uint64_t match; std::uint32_t shares; std::uint32_t price; char stock[9]; char side; };
struct CrossTrade { std::uint64_t shares; std::uint64_t match; std::uint32_t price; char stock[9]; char cross_type; };
struct BrokenTrade { std::uint64_t match; };

struct Message {
  char type;
  Header h;
  union {
    SystemEvent sys;
    StockDirectory dir;
    TradingAction action;
    AddOrder add;
    OrderExecuted exec;
    OrderCancel cancel;
    OrderDelete del;
    OrderReplace replace;
    Trade trade;
    CrossTrade cross;
    BrokenTrade broken;
  };
};

// Fixed wire length including the type byte; -1 for types we do not parse.
constexpr int expected_len(char t) {
  switch (t) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    default: return -1;
  }
}

enum class Parse : std::uint8_t { ok, unknown, malformed };

inline void copy_stock(char (&dst)[9], const std::uint8_t* p) {
  std::memcpy(dst, p, 8);
  dst[8] = '\0';
}

// p points at the type byte of one framed message of n bytes.
inline Parse parse(const std::uint8_t* p, std::size_t n, Message& m) {
  if (n == 0) return Parse::malformed;
  const char t = static_cast<char>(p[0]);
  const int want = expected_len(t);
  if (want < 0) return Parse::unknown;
  if (n != static_cast<std::size_t>(want)) return Parse::malformed;
  m = Message{};
  m.type = t;
  m.h.locate = be16(p + 1);
  m.h.tracking = be16(p + 3);
  m.h.ts = be48(p + 5);
  switch (t) {
    case 'S':
      m.sys.code = static_cast<char>(p[11]);
      break;
    case 'R':
      copy_stock(m.dir.stock, p + 11);
      m.dir.round_lot = be32(p + 21);
      break;
    case 'H':
      copy_stock(m.action.stock, p + 11);
      m.action.state = static_cast<char>(p[19]);
      break;
    case 'A':
    case 'F':
      m.add.ref = be64(p + 11);
      m.add.side = static_cast<char>(p[19]);
      m.add.shares = be32(p + 20);
      copy_stock(m.add.stock, p + 24);
      m.add.price = be32(p + 32);
      m.add.has_mpid = t == 'F';
      if (t == 'F') { std::memcpy(m.add.mpid, p + 36, 4); m.add.mpid[4] = '\0'; }
      break;
    case 'E':
    case 'C':
      m.exec.ref = be64(p + 11);
      m.exec.shares = be32(p + 19);
      m.exec.match = be64(p + 23);
      m.exec.has_price = t == 'C';
      if (t == 'C') {
        m.exec.printable = static_cast<char>(p[31]);
        m.exec.price = be32(p + 32);
      }
      break;
    case 'X':
      m.cancel.ref = be64(p + 11);
      m.cancel.shares = be32(p + 19);
      break;
    case 'D':
      m.del.ref = be64(p + 11);
      break;
    case 'U':
      m.replace.orig = be64(p + 11);
      m.replace.fresh = be64(p + 19);
      m.replace.shares = be32(p + 27);
      m.replace.price = be32(p + 31);
      break;
    case 'P':
      m.trade.ref = be64(p + 11);
      m.trade.side = static_cast<char>(p[19]);
      m.trade.shares = be32(p + 20);
      copy_stock(m.trade.stock, p + 24);
      m.trade.price = be32(p + 32);
      m.trade.match = be64(p + 36);
      break;
    case 'Q':
      m.cross.shares = be64(p + 11);
      copy_stock(m.cross.stock, p + 19);
      m.cross.price = be32(p + 27);
      m.cross.match = be64(p + 31);
      m.cross.cross_type = static_cast<char>(p[39]);
      break;
    case 'B':
      m.broken.match = be64(p + 11);
      break;
    default:
      return Parse::unknown;
  }
  return Parse::ok;
}

// Streaming reader over the standard 2 byte big endian length prefixed format.
class Reader {
 public:
  // msg: parsed; unknown: skipped by length; bad_msg: framed but unparsable; truncated: short read.
  enum class Status : std::uint8_t { msg, unknown, bad_msg, truncated, eof };

  explicit Reader(std::FILE* f) : f_(f) {}

  Status next(Message& m) {
    std::uint8_t lenb[2];
    const std::size_t r = std::fread(lenb, 1, 2, f_);
    if (r == 0) return Status::eof;
    if (r != 2) return Status::truncated;
    const std::size_t len = be16(lenb);
    bytes_ += 2 + len;
    if (len > sizeof(buf_)) {
      if (!skip(len)) return Status::truncated;
      ++unknown_;
      return Status::unknown;
    }
    if (std::fread(buf_, 1, len, f_) != len) return Status::truncated;
    const Parse st = parse(buf_, len, m);
    if (st == Parse::ok) { ++messages_; return Status::msg; }
    if (st == Parse::unknown) { ++unknown_; return Status::unknown; }
    return Status::bad_msg;
  }

  std::uint64_t messages() const { return messages_; }
  std::uint64_t unknown() const { return unknown_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  bool skip(std::size_t n) {
    while (n > 0) {
      const std::size_t chunk = n < sizeof(buf_) ? n : sizeof(buf_);
      if (std::fread(buf_, 1, chunk, f_) != chunk) return false;
      n -= chunk;
    }
    return true;
  }

  std::FILE* f_;
  std::uint8_t buf_[64];
  std::uint64_t messages_{0};
  std::uint64_t unknown_{0};
  std::uint64_t bytes_{0};
};

// Exec sidecar file: 8 byte magic then packed 48 byte ExecRecord entries.
inline constexpr char kExecMagic[8] = {'G', 'V', 'L', 'E', 'X', 'E', 'C', '1'};

inline constexpr std::uint8_t kExecKindE = 0;
inline constexpr std::uint8_t kExecKindC = 1;
inline constexpr std::uint8_t kExecFlagSubPenny = 1;

#pragma pack(push, 1)
struct ExecRecord {
  std::uint64_t gavel_id;   // gavel order id of the executed resting order
  std::uint64_t itch_ref;   // venue order reference number
  std::uint64_t ts;         // itch timestamp of the execution
  std::uint64_t after_seq;  // last stream seq written before this execution
  std::int32_t qty;         // executed shares
  std::int32_t price;       // half ticks: resting price for E, print price for C
  std::uint8_t kind;        // kExecKindE or kExecKindC
  std::uint8_t flags;       // kExecFlagSubPenny when a C print price was not a whole cent
  std::uint8_t pad[6];
};
#pragma pack(pop)
static_assert(sizeof(ExecRecord) == 48);

class ExecWriter {
 public:
  explicit ExecWriter(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
    if (f_) std::fwrite(kExecMagic, 1, 8, f_);
  }
  ~ExecWriter() { close(); }
  ExecWriter(const ExecWriter&) = delete;
  ExecWriter& operator=(const ExecWriter&) = delete;

  bool ok() const { return f_ != nullptr; }
  void write(const ExecRecord& r) {
    std::fwrite(&r, sizeof(r), 1, f_);
    ++count_;
  }
  std::uint64_t count() const { return count_; }
  void close() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
  }

 private:
  std::FILE* f_{nullptr};
  std::uint64_t count_{0};
};

class ExecReader {
 public:
  explicit ExecReader(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
    char magic[8];
    if (f_ && (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, kExecMagic, 8) != 0)) {
      std::fclose(f_);
      f_ = nullptr;
    }
  }
  ~ExecReader() {
    if (f_) std::fclose(f_);
  }
  ExecReader(const ExecReader&) = delete;
  ExecReader& operator=(const ExecReader&) = delete;

  bool ok() const { return f_ != nullptr; }
  bool read(ExecRecord& r) { return std::fread(&r, sizeof(r), 1, f_) == 1; }

 private:
  std::FILE* f_{nullptr};
};

}  // namespace gavel::itch
