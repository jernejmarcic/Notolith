// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QList>
#include <QString>

namespace noteAppC {

/// fzf-style subsequence scoring for the quick switcher, the `[[` suggester
/// and, later, the command palette. No library: the whole rule set is the
/// four bonuses below.
namespace Fuzzy {

struct Match {
    bool matched = false;
    int score = 0;
    QList<qsizetype> positions; ///< indices in `text` that the query hit
};

/// Case-insensitive. An empty query matches everything with score 0.
Match score(const QString &query, const QString &text);

/// True when `text` contains every character of `query` in order.
inline bool matches(const QString &query, const QString &text)
{
    return score(query, text).matched;
}

} // namespace Fuzzy
} // namespace noteAppC
