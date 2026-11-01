// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// QTRY_VERIFY and friends report through QTest's own logger, which a gtest
// binary never starts: a timeout only returns from the test, and it passes.
// These wait the same way and fail the gtest test instead.

#include <QTest>

#include <gtest/gtest.h>

#define WAIT_TRUE(expr, timeoutMs) \
    ASSERT_TRUE(QTest::qWaitFor([&] { return bool(expr); }, timeoutMs)) << "timed out waiting for " #expr

#define WAIT_EQ(actual, expected, timeoutMs)                                            \
    do {                                                                                \
        QTest::qWaitFor([&] { return (actual) == (expected); }, timeoutMs);             \
        ASSERT_EQ(actual, expected);                                                    \
    } while (false)
