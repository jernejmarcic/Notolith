// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace noteAppC {

/// A stable locator into a book: which chapter, and the character range
/// within it.
///
/// This is the "documented subset" of an EPUB CFI the task allows in place
/// of the full spec (idpf.org/epub/linking/cfi/): spine index plus a
/// character range, rather than CFI's step-indexed element path. It is exact
/// rather than approximate precisely because a book's bytes never change -
/// see Vault's own rule, which this project already holds for notes and this
/// module holds for a book - so the same chapter, parsed and laid out by the
/// same code, always produces the same "reading-order text" (see
/// HtmlRenderer::TextRun) and the same offsets into it. There is no
/// re-flow to survive, which is the whole reason CFI's heavier addressing
/// exists.
struct EpubLocator {
    int spineIndex = -1;
    qsizetype start = 0;
    qsizetype end = 0;

    bool isValid() const { return spineIndex >= 0 && end > start; }

    /// "epub:<spineIndex>:<start>-<end>" - also the fragment half of a link
    /// into the book, e.g. "book.epub#epub:0:128-172".
    QString toString() const;
    static std::optional<EpubLocator> parse(const QString &text);

    friend bool operator==(const EpubLocator &a, const EpubLocator &b)
    {
        return a.spineIndex == b.spineIndex && a.start == b.start && a.end == b.end;
    }
};

/// One highlight: where (locator), how it is shown (color, one of a fixed
/// small set of names - see kHighlightColors in the .cpp), what it quotes
/// (for a human reading the sidecar, and as a sanity check on re-anchoring),
/// and the reader's own note about it.
struct EpubHighlight {
    EpubLocator locator;
    QString color;
    QString quote;
    QString note;
};

/// Reads and writes the sidecar Markdown note that holds a book's highlights
/// - "<book, without its extension> highlights.md", beside the .epub (see
/// the report on why beside the book rather than the vault's configurable
/// attachment folder). The note is the source of truth: EpubView loads a
/// book's highlights by parsing it, and every edit (add, recolour, delete,
/// edit the note text) is written straight back - there is no separate
/// database and nothing to lose to a crash between edits.
class EpubHighlights {
public:
    static QString sidecarPath(const QString &bookPath);

    /// Every `%% epub-highlight: ... %%` entry found anywhere in `markdown`,
    /// in file order - regardless of whether it sits inside the generated
    /// section, so a highlight the user relocated by hand while editing is
    /// not silently dropped by the next extraction.
    static QList<EpubHighlight> parse(const QString &markdown);

    /// Rewrites the sidecar at `sidecarPath` from `live` (the reader's
    /// current in-memory list) plus whatever the file on disk already holds
    /// outside `<!-- epub:generated:start/end -->` - created fresh, markers
    /// and all, the first time. Within the generated section, a highlight
    /// whose locator matches one already on disk keeps that entry's note
    /// text verbatim (round-tripping free text the user wrote there); a new
    /// locator gets an empty note to fill in. `chapterTitle(spineIndex)`
    /// supplies the heading each chapter's highlights are grouped under.
    /// False with `error` set on a write failure.
    static bool writeSidecar(const QString &sidecarPath, const QString &bookFileName, const QString &bookTitle,
                             const QList<EpubHighlight> &live, const std::function<QString(int)> &chapterTitle,
                             QString *error = nullptr);

    /// The few names a highlight's colour may be, and the order they are
    /// offered in - e.g. in a colour-picker menu.
    static const QList<QString> &colorNames();
};

} // namespace noteAppC
