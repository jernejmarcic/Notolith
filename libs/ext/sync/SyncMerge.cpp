// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "SyncMerge.h"

#include <algorithm>
#include <vector>

#include <QStringList>

namespace noteAppC {

namespace {

enum class Kind { Keep, Remove, Add };
struct Op {
    Kind kind;
    QString text;
};

/// Same LCS line matcher as RecoveryDialog::lineDiff (libs/ui/RecoveryDialog.cpp):
/// common head/tail trimmed first, then an O(n*m) LCS table over what is
/// left, capped the same way. A copy, not a shared call - see SyncMerge.h.
QList<Op> lineDiff(const QStringList &before, const QStringList &after)
{
    qsizetype head = 0;
    while (head < before.size() && head < after.size() && before.at(head) == after.at(head))
        ++head;
    qsizetype tail = 0;
    while (tail < before.size() - head && tail < after.size() - head
           && before.at(before.size() - 1 - tail) == after.at(after.size() - 1 - tail))
        ++tail;
    const QStringList a = before.mid(head, before.size() - head - tail);
    const QStringList b = after.mid(head, after.size() - head - tail);

    QList<Op> out;
    for (qsizetype i = 0; i < head; ++i)
        out.append({Kind::Keep, before.at(i)});

    constexpr qsizetype kCap = 4'000'000;
    if (!a.isEmpty() && !b.isEmpty() && (a.size() + 1) * (b.size() + 1) <= kCap) {
        const qsizetype rows = a.size() + 1;
        const qsizetype cols = b.size() + 1;
        std::vector<quint32> lcs(size_t(rows * cols), 0);
        for (qsizetype i = a.size() - 1; i >= 0; --i) {
            for (qsizetype j = b.size() - 1; j >= 0; --j) {
                lcs[size_t(i * cols + j)] = a.at(i) == b.at(j)
                                                ? lcs[size_t((i + 1) * cols + j + 1)] + 1
                                                : std::max(lcs[size_t((i + 1) * cols + j)], lcs[size_t(i * cols + j + 1)]);
            }
        }
        qsizetype i = 0;
        qsizetype j = 0;
        while (i < a.size() && j < b.size()) {
            if (a.at(i) == b.at(j)) {
                out.append({Kind::Keep, a.at(i)});
                ++i;
                ++j;
            } else if (lcs[size_t((i + 1) * cols + j)] >= lcs[size_t(i * cols + j + 1)]) {
                out.append({Kind::Remove, a.at(i++)});
            } else {
                out.append({Kind::Add, b.at(j++)});
            }
        }
        for (; i < a.size(); ++i)
            out.append({Kind::Remove, a.at(i)});
        for (; j < b.size(); ++j)
            out.append({Kind::Add, b.at(j)});
    } else {
        for (const QString &line : a)
            out.append({Kind::Remove, line});
        for (const QString &line : b)
            out.append({Kind::Add, line});
    }
    for (qsizetype i = before.size() - tail; i < before.size(); ++i)
        out.append({Kind::Keep, before.at(i)});
    return out;
}

/// A maximal run of base lines [start, end) that `other` replaced with
/// `replacement`. Base lines outside every hunk are unchanged in `other`.
struct Hunk {
    qsizetype start = 0;
    qsizetype end = 0;
    QStringList replacement;
};

QList<Hunk> hunksOf(const QStringList &base, const QStringList &other)
{
    const QList<Op> ops = lineDiff(base, other);
    QList<Hunk> hunks;
    qsizetype baseCursor = 0;
    qsizetype i = 0;
    while (i < ops.size()) {
        if (ops.at(i).kind == Kind::Keep) {
            ++baseCursor;
            ++i;
            continue;
        }
        Hunk hunk;
        hunk.start = baseCursor;
        while (i < ops.size() && ops.at(i).kind != Kind::Keep) {
            if (ops.at(i).kind == Kind::Remove)
                ++baseCursor;
            else
                hunk.replacement << ops.at(i).text;
            ++i;
        }
        hunk.end = baseCursor;
        hunks.append(hunk);
    }
    return hunks;
}

} // namespace

SyncMerge::Result SyncMerge::merge(const QString &base, const QString &local, const QString &remote)
{
    const QStringList baseLines = base.split(QLatin1Char('\n'));
    const QStringList localLines = local.split(QLatin1Char('\n'));
    const QStringList remoteLines = remote.split(QLatin1Char('\n'));

    if (local == remote)
        return {true, local}; // both sides landed on the same text - never a conflict

    const QList<Hunk> localHunks = hunksOf(baseLines, localLines);
    const QList<Hunk> remoteHunks = hunksOf(baseLines, remoteLines);

    // Interval-merge every hunk from both sides into maximal overlapping
    // groups, so two edits that touch overlapping base ranges (one from
    // each side) are resolved together rather than compared piecewise.
    struct Interval {
        qsizetype start, end;
    };
    QList<Interval> all;
    for (const Hunk &h : localHunks)
        all.append({h.start, h.end});
    for (const Hunk &h : remoteHunks)
        all.append({h.start, h.end});
    // A pure insertion has start == end (a zero-width point); sort by start
    // then treat overlap as start < otherEnd (not <=), so two insertions at
    // the same point still merge into one group instead of silently
    // ordering one before the other.
    std::sort(all.begin(), all.end(), [](const Interval &x, const Interval &y) { return x.start < y.start; });
    QList<Interval> groups;
    for (const Interval &iv : all) {
        if (!groups.isEmpty() && iv.start <= groups.last().end) {
            groups.last().end = std::max(groups.last().end, iv.end);
        } else {
            groups.append(iv);
        }
    }

    QString merged;
    qsizetype pos = 0;
    bool clean = true;
    const auto appendLines = [&merged](const QStringList &lines) {
        for (const QString &line : lines) {
            merged += line;
            merged += QLatin1Char('\n');
        }
    };
    const auto textFor = [&baseLines](const QList<Hunk> &hunks, qsizetype from, qsizetype to) {
        // Base range [from, to) as `hunks`'s side rendered it: any hunk
        // fully inside contributes its replacement, gaps between hunks (or
        // before/after them, within the range) are unchanged base text.
        QStringList out;
        qsizetype cursor = from;
        for (const Hunk &h : hunks) {
            if (h.start < from || h.end > to)
                continue; // belongs to a different (or no) group
            for (qsizetype i = cursor; i < h.start; ++i)
                out << baseLines.at(i);
            out << h.replacement;
            cursor = h.end;
        }
        for (qsizetype i = cursor; i < to; ++i)
            out << baseLines.at(i);
        return out;
    };

    for (const Interval &group : groups) {
        for (qsizetype i = pos; i < group.start; ++i)
            merged += baseLines.at(i) + QLatin1Char('\n');
        const QStringList baseSlice = baseLines.mid(group.start, group.end - group.start);
        const QStringList localSlice = textFor(localHunks, group.start, group.end);
        const QStringList remoteSlice = textFor(remoteHunks, group.start, group.end);
        if (localSlice == baseSlice) {
            appendLines(remoteSlice);
        } else if (remoteSlice == baseSlice) {
            appendLines(localSlice);
        } else if (localSlice == remoteSlice) {
            appendLines(localSlice);
        } else {
            clean = false;
        }
        pos = group.end;
    }
    for (qsizetype i = pos; i < baseLines.size(); ++i)
        merged += baseLines.at(i) + QLatin1Char('\n');

    if (!clean)
        return {false, {}};
    if (merged.endsWith(QLatin1Char('\n')))
        merged.chop(1); // split('\n') + join added one trailing entry too many
    return {true, merged};
}

} // namespace noteAppC
