#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "doctest.h"
#include "../bench/histogram.hpp"

using gbench::Histogram;

namespace {

struct Rng {
  std::uint64_t x;
  std::uint64_t next() {
    x += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  double unit() { return (static_cast<double>(next() >> 11) + 1.0) * 0x1p-53; }
};

std::uint64_t exact_pct(const std::vector<std::uint64_t>& sorted, double p) {
  double rank = std::ceil(p / 100.0 * static_cast<double>(sorted.size()));
  if (rank < 1.0) rank = 1.0;
  return sorted[static_cast<std::size_t>(rank) - 1];
}

void check_against_exact(const Histogram& h, std::vector<std::uint64_t> v, double tol) {
  std::sort(v.begin(), v.end());
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const double e = static_cast<double>(exact_pct(v, p));
    const double a = static_cast<double>(h.percentile(p));
    CHECK(std::fabs(a - e) <= tol * e + 1.0);
  }
  CHECK(h.count() == v.size());
  CHECK(h.max() == v.back());
  CHECK(h.percentile(100.0) == v.back());
}

}  // namespace

TEST_CASE("uniform distribution percentiles within precision") {
  Rng r{42};
  Histogram h;
  std::vector<std::uint64_t> v;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t x = 1 + r.next() % 1'000'000;
    h.record(x);
    v.push_back(x);
  }
  check_against_exact(h, v, 0.01);
}

TEST_CASE("lognormal like distribution percentiles within precision") {
  Rng r{7};
  Histogram h;
  std::vector<std::uint64_t> v;
  for (int i = 0; i < 200000; ++i) {
    const double z = std::sqrt(-2.0 * std::log(r.unit())) * std::cos(6.2831853071795865 * r.unit());
    double x = std::exp(11.0 + 1.5 * z);
    if (x < 1.0) x = 1.0;
    const std::uint64_t ns = static_cast<std::uint64_t>(x);
    h.record(ns);
    v.push_back(ns);
  }
  check_against_exact(h, v, 0.01);
}

TEST_CASE("point mass is exact at every percentile") {
  Histogram h;
  for (int i = 0; i < 1000; ++i) h.record(777777);
  for (double p : {0.0, 50.0, 99.0, 99.99, 100.0}) CHECK(h.percentile(p) == 777777);
  CHECK(h.max() == 777777);
  CHECK(h.min() == 777777);
  CHECK(h.count() == 1000);
}

TEST_CASE("merge equals recording everything into one histogram") {
  Rng r{99};
  Histogram a, b, all;
  for (int i = 0; i < 50000; ++i) {
    const std::uint64_t x = 1 + r.next() % 10'000'000;
    all.record(x);
    if (i % 2) a.record(x);
    else b.record(x);
  }
  a.merge(b);
  CHECK(a.count() == all.count());
  CHECK(a.max() == all.max());
  CHECK(a.min() == all.min());
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99, 100.0})
    CHECK(a.percentile(p) == all.percentile(p));
}

TEST_CASE("edge values") {
  Histogram h;
  CHECK(h.count() == 0);
  CHECK(h.max() == 0);
  CHECK(h.percentile(50.0) == 0);

  h.record(0);
  h.record(1);
  CHECK(h.count() == 2);
  CHECK(h.min() == 0);
  CHECK(h.max() == 1);
  CHECK(h.percentile(0.0) == 0);
  CHECK(h.percentile(100.0) == 1);

  // Above range values clamp to 100 s.
  h.record(2 * Histogram::kMaxValue);
  CHECK(h.max() == Histogram::kMaxValue);
  CHECK(h.percentile(100.0) == Histogram::kMaxValue);

  // Linear region values below 128 ns are exact.
  Histogram lin;
  lin.record(63);
  lin.record(64);
  lin.record(127);
  CHECK(lin.percentile(0.0) == 63);
  CHECK(lin.percentile(50.0) == 64);
  CHECK(lin.percentile(100.0) == 127);
}
