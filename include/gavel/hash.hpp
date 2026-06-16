#pragma once
#include <cstdint>
#include <cstddef>

namespace gavel {

inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ull;

constexpr std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t h = kFnvOffset) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= kFnvPrime; }
  return h;
}

// splitmix64 style mixer for the incremental book digest.
constexpr std::uint64_t mix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

}  // namespace gavel
