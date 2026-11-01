// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// libFuzzer target: the CSV/TSV parser behind `![[data.csv]]`, with the
// limits the embed uses and without.

#include <QByteArray>

#include "CSVparser.h"

using namespace noteAppC;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0)
        return 0;
    // The first byte picks the limits the embed uses or none; the rest is the file.
    const bool limited = data[0] & 1;
    const QByteArray bytes(reinterpret_cast<const char *>(data) + 1, static_cast<qsizetype>(size) - 1);
    const QString text = decodeCSV(bytes, QLocale::English);
    const QChar delimiter = CSVdelimiter(text);
    if (limited)
        parseCSV(text, delimiter, 200, 50, 500);
    else
        parseCSV(text, delimiter);
    return 0;
}
