#include <cstdint>
#include <thread>
#include "doctest.h"
#include "gavel/spsc.hpp"

using namespace gavel;

TEST_CASE("spsc fills to capacity and rejects when full") {
  SpscRing<std::uint64_t> r(8);
  CHECK(r.capacity() == 8);
  for (std::uint64_t i = 0; i < 8; ++i) REQUIRE(r.try_push(i));
  std::uint64_t v = 0;
  CHECK_FALSE(r.try_push(99));
  REQUIRE(r.try_pop(v));
  CHECK(v == 0);
  // One slot freed; exactly one push fits again.
  CHECK(r.try_push(8));
  CHECK_FALSE(r.try_push(9));
}

TEST_CASE("spsc pops in fifo order and empties") {
  SpscRing<std::uint64_t> r(8);
  for (std::uint64_t i = 0; i < 5; ++i) REQUIRE(r.try_push(i * 10));
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 5; ++i) {
    REQUIRE(r.try_pop(v));
    CHECK(v == i * 10);
  }
  CHECK_FALSE(r.try_pop(v));
}

TEST_CASE("spsc wraps the index correctly") {
  SpscRing<std::uint64_t> r(4);
  std::uint64_t v = 0;
  std::uint64_t next = 0;
  // Cycle far past capacity with a partially full ring.
  for (int round = 0; round < 1000; ++round) {
    REQUIRE(r.try_push(static_cast<std::uint64_t>(round) * 3));
    REQUIRE(r.try_push(static_cast<std::uint64_t>(round) * 3 + 1));
    REQUIRE(r.try_push(static_cast<std::uint64_t>(round) * 3 + 2));
    for (int k = 0; k < 3; ++k) {
      REQUIRE(r.try_pop(v));
      REQUIRE(v == next);
      ++next;
    }
  }
  CHECK_FALSE(r.try_pop(v));
}

TEST_CASE("spsc two thread stress preserves order and completeness") {
  constexpr std::uint64_t kN = 5000000;
  SpscRing<std::uint64_t> r(1024);
  std::uint64_t bad = 0;
  std::uint64_t received = 0;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN; ++i) {
      while (!r.try_push(i)) {}
    }
  });
  std::thread consumer([&] {
    std::uint64_t v = 0;
    for (std::uint64_t expect = 0; expect < kN; ++expect) {
      while (!r.try_pop(v)) {}
      if (v != expect) ++bad;
      ++received;
    }
  });
  producer.join();
  consumer.join();
  CHECK(bad == 0);
  CHECK(received == kN);
  std::uint64_t v = 0;
  CHECK_FALSE(r.try_pop(v));
}
