// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QList>
#include <QString>

#include "SourceMap.h" // Range's neighbour; qsizetype offsets, the way the rest of the editor counts

namespace noteAppC {

/// One match inside a note a ```query``` block's search found. Mirrors
/// libs/index/Search.h's SearchMatch field for field, so the window's
/// Index::search() -> QueryResults adapter is a plain copy - see
/// MarkdownEditor::setQueryRunner. libs/editor never includes libs/index
/// (docs/PLUGINS.md's module boundary), so the shape is repeated here rather
/// than shared.
struct QueryMatch {
    qsizetype offset = 0;
    qsizetype length = 0;
};

/// One note a ```query``` block's search matched, mirroring libs/index's
/// SearchResult. `snippet`/`snippetHighlights` are already built (Search.h's
/// buildSnippet) so the editor never re-reads the note to show one.
struct QueryResult {
    QString path; ///< vault-relative
    int matchCount = 0;
    QString snippet;
    QList<QueryMatch> snippetHighlights; ///< offsets into `snippet`
    /// The first match's offset into the note's own text, so clicking a
    /// result can open the note at that point - see
    /// MarkdownEditor::queryResultActivated.
    qsizetype firstOffset = 0;
};

using QueryResults = QList<QueryResult>;

} // namespace noteAppC
