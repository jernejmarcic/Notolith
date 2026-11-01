// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// Helpers for the tests that guard a wall-clock budget.
//
// A busy machine (parallel builds, another test run next door) only ever adds
// time, so the fastest of a few runs is the cost of the code, not of its
// neighbours: take that, and keep the budget itself unchanged so a real
// regression still fails. Under a sanitizer every access is instrumented and
// the clock measures the instrumentation, so those guards skip.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NOTEAPPC_SANITIZED_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define NOTEAPPC_SANITIZED_BUILD 1
#endif
#endif

#ifdef NOTEAPPC_SANITIZED_BUILD
#define SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER() \
    GTEST_SKIP() << "wall-clock budget: a sanitizer's instrumentation, not the code, would be measured"
#else
#define SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER() ((void)0)
#endif

namespace perf {

/// The smallest value `measure()` returns over up to `tries` runs, stopping
/// early once one is at or under `goodEnough` (a passing run needs no repeat).
template <typename Measure>
auto bestOf(Measure &&measure, int tries = 3,
            double goodEnough = -std::numeric_limits<double>::infinity())
{
    auto best = measure();
    for (int run = 1; run < tries && double(best) > goodEnough; ++run)
        best = std::min(best, measure());
    return best;
}

} // namespace perf
