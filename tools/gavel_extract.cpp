#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "gavel/itch/extract.hpp"
#include "gavel/stream.hpp"

// Reads raw uncompressed ITCH 5.0 from stdin and extracts gavel input streams per symbol.
namespace {

using gavel::itch::Message;
using gavel::itch::Reader;

struct SymbolOut {
  std::string name;
  gavel::itch::SymbolMapper mapper;
  std::unique_ptr<gavel::StreamWriter> stream;
  std::unique_ptr<gavel::itch::ExecWriter> exec;
  std::uint64_t stream_records{0};
  std::uint64_t exec_records{0};
  bool bound{false};
};

int usage() {
  std::fprintf(stderr, "usage: gavel-extract --symbols AAPL,MSFT --out DIR [--stats] < tape.itch\n");
  std::fprintf(stderr, "       gavel-extract --stats [--symbols AAPL,MSFT] < tape.itch\n");
  return 2;
}

std::vector<std::string> split_csv(const char* s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* p = s; ; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      if (*p == '\0') break;
    } else {
      cur.push_back(*p);
    }
  }
  return out;
}

const char* stock_of(const Message& m) {
  switch (m.type) {
    case 'R': return m.dir.stock;
    case 'H': return m.action.stock;
    case 'A': case 'F': return m.add.stock;
    case 'P': return m.trade.stock;
    case 'Q': return m.cross.stock;
    default: return nullptr;
  }
}

unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> symbols;
  std::string out_dir;
  bool stats = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) symbols = split_csv(argv[++i]);
    else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    else if (std::strcmp(argv[i], "--stats") == 0) stats = true;
    else return usage();
  }
  if (out_dir.empty() && !stats) return usage();
  if (!out_dir.empty() && symbols.empty()) return usage();

  std::vector<SymbolOut> outs;
  outs.reserve(symbols.size());
  std::unordered_map<std::string, std::size_t> by_name;
  for (const auto& s : symbols) {
    SymbolOut o;
    o.name = s;
    if (!out_dir.empty()) {
      o.stream = std::make_unique<gavel::StreamWriter>(out_dir + "/" + s + ".gvl");
      o.exec = std::make_unique<gavel::itch::ExecWriter>(out_dir + "/" + s + ".exec");
      if (!o.stream->ok() || !o.exec->ok()) {
        std::fprintf(stderr, "cannot open outputs for %s in %s\n", s.c_str(), out_dir.c_str());
        return 1;
      }
    }
    by_name.emplace(s, outs.size());
    outs.push_back(std::move(o));
  }

  std::unordered_map<std::uint16_t, std::size_t> by_locate;
  std::uint64_t g_by_type[256] = {};
  std::uint64_t g_total = 0, g_unknown = 0, g_bad = 0, g_sub_penny = 0, g_truncated = 0;

  Reader rd(stdin);
  Message m;
  for (bool done = false; !done;) {
    switch (rd.next(m)) {
      case Reader::Status::eof: done = true; continue;
      case Reader::Status::truncated: ++g_truncated; done = true; continue;
      case Reader::Status::unknown: ++g_unknown; continue;
      case Reader::Status::bad_msg: ++g_bad; continue;
      case Reader::Status::msg: break;
    }
    ++g_total;
    ++g_by_type[static_cast<unsigned char>(m.type)];
    if ((m.type == 'A' || m.type == 'F') && !gavel::itch::whole_cent(m.add.price)) ++g_sub_penny;
    else if (m.type == 'U' && !gavel::itch::whole_cent(m.replace.price)) ++g_sub_penny;

    if (const char* st = stock_of(m); st && !by_locate.contains(m.h.locate)) {
      auto it = by_name.find(gavel::itch::trim_stock(st));
      if (it != by_name.end()) {
        by_locate.emplace(m.h.locate, it->second);
        outs[it->second].bound = true;
      }
    }
    auto lit = by_locate.find(m.h.locate);
    if (lit == by_locate.end()) continue;
    SymbolOut& so = outs[lit->second];
    gavel::itch::MapOut mo;
    so.mapper.map(m, mo);
    for (std::uint32_t i = 0; i < mo.nmsgs; ++i) {
      if (so.stream) so.stream->write(mo.msgs[i]);
      ++so.stream_records;
    }
    if (mo.has_exec) {
      if (so.exec) so.exec->write(mo.exec);
      ++so.exec_records;
    }
  }
  for (auto& o : outs) {
    if (o.stream) o.stream->close();
    if (o.exec) o.exec->close();
  }

  std::fprintf(stderr, "tape: %llu messages, %llu bytes, %llu unknown skipped, %llu bad, %llu truncated\n",
               ull(g_total), ull(rd.bytes()), ull(g_unknown), ull(g_bad), ull(g_truncated));
  std::fprintf(stderr, "tape sub penny prices (A/F/U): %llu\n", ull(g_sub_penny));
  std::fprintf(stderr, "tape counts by type:");
  for (unsigned t = 0; t < 256; ++t)
    if (g_by_type[t]) std::fprintf(stderr, " %c=%llu", static_cast<char>(t), ull(g_by_type[t]));
  std::fprintf(stderr, "\n");
  for (const auto& o : outs) {
    const auto& c = o.mapper.counters;
    std::fprintf(stderr, "%s: %s, %llu stream records, %llu exec records, %llu live refs at eof\n",
                 o.name.c_str(), o.bound ? "bound" : "not seen on tape",
                 ull(o.stream_records), ull(o.exec_records), ull(o.mapper.live_refs()));
    std::fprintf(stderr, "%s: sub penny skips %llu, orphan refs %llu, counts:",
                 o.name.c_str(), ull(c.sub_penny), ull(c.orphan_ref));
    for (unsigned t = 0; t < 256; ++t)
      if (c.by_type[t]) std::fprintf(stderr, " %c=%llu", static_cast<char>(t), ull(c.by_type[t]));
    std::fprintf(stderr, "\n");
  }
  return 0;
}
