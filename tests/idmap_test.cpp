#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>
#include "doctest.h"
#include "gavel/idmap.hpp"
#include "gavel/hash.hpp"

using namespace gavel;

namespace {

// Finds a key whose home slot equals want for a table with the given mask.
std::uint64_t key_for_slot(std::size_t want, std::size_t mask, std::uint64_t start = 1) {
  for (std::uint64_t k = start;; ++k) {
    if ((mix64(k) & mask) == want) return k;
  }
}

}  // namespace

TEST_CASE("idmap basic insert find erase") {
  IdMap m(4);
  CHECK(m.find(42) == IdMap::kEmpty);
  m.insert(42, 7);
  CHECK(m.find(42) == 7);
  CHECK(m.size() == 1);
  m.insert(42, 9);
  CHECK(m.find(42) == 9);
  CHECK(m.size() == 1);
  m.erase(42);
  CHECK(m.find(42) == IdMap::kEmpty);
  CHECK(m.size() == 0);
  // Erase of an absent key is a no op.
  m.erase(42);
  CHECK(m.size() == 0);
}

TEST_CASE("idmap backward shift erase across wraparound") {
  // Cap 16 mask 15; stay under the growth threshold of 11 entries.
  const std::size_t mask = 15;
  // Three keys homed at slot 14 and one homed at 15: chain occupies 14, 15, 0, 1.
  const std::uint64_t a = key_for_slot(14, mask);
  const std::uint64_t b = key_for_slot(14, mask, a + 1);
  const std::uint64_t c = key_for_slot(14, mask, b + 1);
  const std::uint64_t d = key_for_slot(15, mask);
  REQUIRE(((mix64(a) & mask) == 14));
  REQUIRE(((mix64(d) & mask) == 15));

  SUBCASE("erase head of a chain spanning the wrap") {
    IdMap m(4);
    m.insert(a, 1);
    m.insert(b, 2);
    m.insert(c, 3);
    m.insert(d, 4);
    m.erase(a);
    CHECK(m.find(a) == IdMap::kEmpty);
    CHECK(m.find(b) == 2);
    CHECK(m.find(c) == 3);
    CHECK(m.find(d) == 4);
    CHECK(m.size() == 3);
  }

  SUBCASE("erase element sitting at the last table slot") {
    IdMap m(4);
    m.insert(a, 1);
    m.insert(b, 2);  // lands at 15
    m.insert(c, 3);  // lands at 0
    m.erase(b);
    CHECK(m.find(a) == 1);
    CHECK(m.find(b) == IdMap::kEmpty);
    CHECK(m.find(c) == 3);
    CHECK(m.size() == 2);
  }

  SUBCASE("erase past the wrap keeps the tail reachable") {
    IdMap m(4);
    m.insert(a, 1);
    m.insert(b, 2);
    m.insert(c, 3);
    m.insert(d, 4);  // lands at 1 after the chain
    m.erase(c);
    CHECK(m.find(a) == 1);
    CHECK(m.find(b) == 2);
    CHECK(m.find(d) == 4);
    CHECK(m.find(c) == IdMap::kEmpty);
    m.erase(b);
    CHECK(m.find(a) == 1);
    CHECK(m.find(d) == 4);
    CHECK(m.size() == 2);
  }

  SUBCASE("erase then reinsert the same keys") {
    IdMap m(4);
    m.insert(a, 1);
    m.insert(b, 2);
    m.insert(c, 3);
    m.insert(d, 4);
    m.erase(a);
    m.erase(d);
    m.insert(a, 10);
    m.insert(d, 40);
    CHECK(m.find(a) == 10);
    CHECK(m.find(b) == 2);
    CHECK(m.find(c) == 3);
    CHECK(m.find(d) == 40);
    CHECK(m.size() == 4);
  }
}

TEST_CASE("idmap growth keeps all entries findable") {
  IdMap m(4);
  const std::uint32_t n = 100000;
  for (std::uint32_t i = 0; i < n; ++i) m.insert(1000000ull + i, i);
  CHECK(m.size() == n);
  for (std::uint32_t i = 0; i < n; ++i) REQUIRE(m.find(1000000ull + i) == i);
  CHECK(m.find(999999) == IdMap::kEmpty);
  CHECK(m.find(1000000ull + n) == IdMap::kEmpty);
  // Erase half, check both halves.
  for (std::uint32_t i = 0; i < n; i += 2) m.erase(1000000ull + i);
  CHECK(m.size() == n / 2);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t got = m.find(1000000ull + i);
    if (i % 2 == 0) REQUIRE(got == IdMap::kEmpty);
    else REQUIRE(got == i);
  }
}

TEST_CASE("idmap fuzz against unordered_map reference") {
  std::mt19937_64 rng(0xC0FFEEull);
  IdMap m(8);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  // Clustered key space keeps load high and probe chains long.
  auto pick_key = [&] {
    const std::uint64_t cluster = (rng() % 8) * 1000003ull;
    return cluster + rng() % 40000;
  };
  std::vector<std::uint64_t> live;
  const int ops = 1000000;
  for (int i = 0; i < ops; ++i) {
    const std::uint64_t r = rng() % 100;
    if (r < 45) {
      const std::uint64_t k = pick_key();
      const auto v = static_cast<std::uint32_t>(rng() & 0x7fffffff);
      if (!ref.count(k)) live.push_back(k);
      ref[k] = v;
      m.insert(k, v);
    } else if (r < 80) {
      // Probe both live and arbitrary keys.
      const std::uint64_t k = (!live.empty() && (rng() & 1))
                                  ? live[rng() % live.size()]
                                  : pick_key();
      const auto it = ref.find(k);
      const std::uint32_t want = it == ref.end() ? IdMap::kEmpty : it->second;
      REQUIRE(m.find(k) == want);
    } else {
      const std::uint64_t k = (!live.empty() && (rng() % 4))
                                  ? live[rng() % live.size()]
                                  : pick_key();
      ref.erase(k);
      m.erase(k);
      REQUIRE(m.find(k) == IdMap::kEmpty);
    }
    if ((i & 0xffff) == 0) REQUIRE(m.size() == ref.size());
  }
  REQUIRE(m.size() == ref.size());
  for (const auto& [k, v] : ref) REQUIRE(m.find(k) == v);
}
