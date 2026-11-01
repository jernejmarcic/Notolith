// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>

namespace noteAppC {

/// Line-based three-way merge for a Markdown body (docs/SYNC.md "Conflict
/// rules"): diff3(base, local, remote). Not a character-level CRDT - that
/// is not implemented (docs/SYNC.md "Not implemented").
///
/// The line matcher is a small, separate copy of the LCS diff already in
/// libs/ui/RecoveryDialog.cpp (lineDiff), extended to run twice (base-vs-
/// local, base-vs-remote) and reconciled. libs/ui cannot be depended on from
/// here, so this is a copy rather than a shared extraction -
/// worth moving to somewhere both can include if a third user of line
/// diffing turns up, not before.
class SyncMerge {
public:
    struct Result {
        bool clean = false; ///< no region was edited differently on both sides
        QString merged;     ///< valid only when clean
    };

    /// `local` and `remote` are each diffed against `base`; a region only
    /// one side touched is taken as-is, a region both touched identically
    /// is taken once, and a region touched differently by both marks the
    /// result unclean (the caller writes a conflict copy - see
    /// docs/SYNC.md).
    static Result merge(const QString &base, const QString &local, const QString &remote);
};

} // namespace noteAppC
