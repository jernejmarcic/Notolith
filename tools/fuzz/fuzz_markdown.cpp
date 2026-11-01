// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// libFuzzer target: the Markdown parse path the editor, the indexer and the
// publisher share, with every option bit the input picks, plus the helpers
// that read a note's own text (table rows, heading sections, the user
// regular expression with its match budget).

#include <QRegularExpression>
#include <QString>

#include <algorithm>

#include "Parser.h"

using namespace noteAppC;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0)
        return 0;
    // The first byte picks the options; the rest is the note, and its first line doubles as the
    // delimiters, the pattern and the heading to look for.
    const uint8_t bits = data[0];
    ParseOptions options;
    options.brackets = bits & 1;
    options.metaNotes = bits & 2;
    options.excludeBetween = bits & 4;
    options.ignoreFormatting = bits & 8;
    options.taskStates = bits & 16;
    options.codeStyler = bits & 32;
    if (options.codeStyler)
        options.highlightKinds = {QStringLiteral("info"), QStringLiteral("warn")};
    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(data) + 1, static_cast<qsizetype>(size) - 1);
    const QString pattern = text.left(std::min<qsizetype>(64, text.indexOf(QLatin1Char('\n'))));
    if (bits & 64) {
        options.open = pattern.left(3);
        options.close = pattern.mid(3, 3);
    }

    Parser parser;
    parser.parseAll(text, QStringLiteral("fuzz.md"), options);
    tableRowCells(text, 0, text.size());
    wordsIn(text);
    headingSection(text, pattern);
    parseHighlightKinds(pattern);
    metaNoteDelimitersError(options.open, options.close);
    if (const auto regex = userRegularExpression(pattern)) {
        auto it = regex->globalMatch(text);
        while (it.hasNext())
            it.next();
    }
    return 0;
}
