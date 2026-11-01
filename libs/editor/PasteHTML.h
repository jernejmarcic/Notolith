// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QString>

namespace noteAppC {

/// Pasted rich text as Markdown, the way Obsidian pastes it: what a browser,
/// Word, LibreOffice or Google Docs puts on the clipboard as text/html, with
/// its headings, emphasis, links, lists, quotes, code, tables and remote
/// images kept. Qt's own QTextDocument does the parsing and the writing; this
/// patches what it gets wrong (see PasteHTML.cpp).
///
/// A null string means "paste `plain` instead": the HTML is over
/// kMaxPasteHTML, converts to nothing, or carries no formatting at all (a
/// terminal's span around plain text), where escaping or reflowing it would
/// only damage the text.
QString HTMLtoMarkdown(const QString &html, const QString &plain);

/// Above this many characters the HTML is not parsed: QTextDocument runs on
/// the UI thread and a hostile page could keep it there.
constexpr qsizetype kMaxPasteHTML = 2 * 1024 * 1024;

} // namespace noteAppC
