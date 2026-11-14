// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// A counter word, `RA.1`, `1RA`, `v09`, `x99`, or a letter on its own, `a)`:
/// the word one step on, or an empty string when the word is no counter. All
/// but the last digit run (or the letter) is kept, and zero padding keeps its
/// width. Shared by bullet continuation and table columns.
QString nextCounterWord(const QString &word);

/// Table columns that count up. `rows` is a table as cell text: the header,
/// the delimiter row, then the body rows (MarkdownEditor::TableText).
namespace TableCounters {

/// The columns that count: the first, or one whose header holds a word like
/// "ID" or "Nr.", and whose body cells are a +1 sequence of one counter
/// pattern. A keyword column with no body text counts and starts at 1; a first
/// column without a keyword needs a number in it. A column the person broke
/// out of step is not one, so nothing touches it: the file is the state.
QList<int> columns(const QList<QStringList> &rows);

/// An empty row was inserted at `line` (at least 2): the counter cells of the
/// rows from there on are numbered, the new one taking the value the slot had.
void afterInsert(QList<QStringList> &rows, int line, const QList<int> &counters);

/// `removed` was taken out of `line`: the rows from there on close up.
void afterDelete(QList<QStringList> &rows, int line, const QStringList &removed, const QList<int> &counters);

} // namespace TableCounters

} // namespace noteAppC
