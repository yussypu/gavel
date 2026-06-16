#pragma once
#include <cstdio>
#include <cstdlib>

// Contract macros for the hardening study. GAVEL_HARDENING 0: no checks. 1: hot path preconditions enforced (abort on violation).
#ifndef GAVEL_HARDENING
#define GAVEL_HARDENING 1
#endif

namespace gavel::detail {
[[noreturn]] inline void contract_fail(const char* expr, const char* file, int line) {
  std::fprintf(stderr, "contract violation: %s at %s:%d\n", expr, file, line);
  std::abort();
}
}  // namespace gavel::detail

#if GAVEL_HARDENING >= 1
#define GAVEL_PRE(cond) \
  ((cond) ? (void)0 : ::gavel::detail::contract_fail(#cond, __FILE__, __LINE__))
#else
#define GAVEL_PRE(cond) ((void)0)
#endif

// Invariant checks that run in all builds; used off the hot path.
#define GAVEL_REQUIRE(cond) \
  ((cond) ? (void)0 : ::gavel::detail::contract_fail(#cond, __FILE__, __LINE__))
