// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>

namespace noteAppC {

/// Aligns a GFM table's source so it reads as a table in plain text, the way
/// Obsidian's Advanced Tables plugin does: pads cells to a common column
/// width and normalises the pipes. Pure text in, text out - no parser
/// involved, so it works equally on a table the editor has not parsed yet.
///
/// `table` is the table's lines, LF-separated (keep a trailing newline if
/// given). Returns `table` unchanged if it is not a table (no valid
/// delimiter row as line 2).
QString formatTable(const QString &table);

} // namespace noteAppC
