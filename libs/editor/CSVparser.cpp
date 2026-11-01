// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "CSVparser.h"

#include <QStringConverter>

#include <algorithm>

namespace noteAppC {

namespace {

QStringView withoutBOM(QStringView text)
{
    return text.startsWith(QChar(0xFEFF)) ? text.mid(1) : text;
}

} // namespace

QChar CSVdelimiter(QStringView text)
{
    text = withoutBOM(text);
    int commas = 0, semicolons = 0, tabs = 0;
    bool quoted = false;
    bool fieldStart = true; // as the parser: only a quote that opens a field is one
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c != QLatin1Char('"'))
                continue;
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"'))
                ++i; // "" is a quote inside the field
            else
                quoted = false;
            continue;
        }
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            break;
        if (c == QLatin1Char('"') && fieldStart)
            quoted = true;
        else if (c == QLatin1Char(','))
            ++commas;
        else if (c == QLatin1Char(';'))
            ++semicolons;
        else if (c == QLatin1Char('\t'))
            ++tabs;
        fieldStart = c == QLatin1Char(',') || c == QLatin1Char(';') || c == QLatin1Char('\t');
    }
    if (semicolons > commas && semicolons >= tabs)
        return QLatin1Char(';');
    if (tabs > commas && tabs > semicolons)
        return QLatin1Char('\t');
    return QLatin1Char(',');
}

CSVtable parseCSV(QStringView text, QChar delimiter, qsizetype maxRows, int maxColumns, int maxCellChars)
{
    text = withoutBOM(text);
    CSVtable table;
    QStringList record;
    QString field;
    int widest = 0;         // the most cells any kept record had, kept or not
    int cells = 0;          // cells ended so far in this record
    bool quoted = false;    // inside a quoted field
    bool fieldStarted = false; // the field being read has had a character, a quote included
    bool cut = false;       // ... and was cut at maxCellChars
    bool pending = false;   // the record has any content at all, so a blank line is not one

    const auto keeping = [&] { return maxColumns < 0 || cells < maxColumns; };
    const auto add = [&](QChar c) {
        fieldStarted = pending = true;
        if (!keeping())
            return; // a column past the cap: parsed for its structure, never stored
        if (maxCellChars >= 0 && field.size() >= maxCellChars)
            cut = true;
        else
            field += c;
    };
    const auto endField = [&] {
        if (keeping()) {
            // a cut can fall between the halves of a surrogate pair: the lone half goes
            if (cut && !field.isEmpty() && field.back().isHighSurrogate())
                field.chop(1);
            record << (cut ? field + QChar(0x2026) : field);
        }
        ++cells;
        field.clear();
        fieldStarted = cut = false;
    };
    const auto endRecord = [&] {
        if (pending) {
            endField();
            ++table.totalRows;
            if (maxRows < 0 || table.rows.size() < maxRows) {
                table.columns = std::max(table.columns, int(record.size()));
                widest = std::max(widest, cells);
                table.rows << record;
            }
        }
        record.clear();
        field.clear();
        cells = 0;
        fieldStarted = cut = pending = false;
    };

    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c != QLatin1Char('"'))
                add(c);
            else if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                add(c);
                ++i;
            } else
                quoted = false;
        } else if (c == QLatin1Char('"') && !fieldStarted) {
            quoted = fieldStarted = pending = true;
        } else if (c == delimiter) {
            endField();
            pending = true;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;
            endRecord();
        } else {
            add(c);
        }
    }
    endRecord();

    table.hiddenColumns = widest - table.columns;
    for (QStringList &padded : table.rows) {
        while (padded.size() < table.columns)
            padded << QString();
    }
    return table;
}

namespace {

// Whether the bytes hold at least one well-formed UTF-8 multibyte sequence:
// a file that does was written as UTF-8, whatever else is wrong with it.
bool hasUTF8multibyte(const QByteArray &bytes)
{
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (data[i] < 0xC2 || data[i] > 0xF4)
            continue;
        const int continuation = data[i] < 0xE0 ? 1 : data[i] < 0xF0 ? 2 : 3;
        if (i + continuation >= bytes.size())
            continue;
        bool valid = true;
        for (int k = 1; k <= continuation && valid; ++k)
            valid = (data[i + k] & 0xC0) == 0x80;
        if (valid)
            return true;
    }
    return false;
}

} // namespace

QString decodeCSV(const QByteArray &bytes, QLocale::Language language)
{
    if (const auto encoding = QStringConverter::encodingForData(bytes))
        return QStringDecoder(*encoding).decode(bytes);
    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8.decode(bytes);
    // A stray bad byte in a UTF-8 file is one replacement character, not a
    // reason to read the whole file as a code page.
    if (!utf8.hasError() || hasUTF8multibyte(bytes))
        return text;

    static const QList<QLocale::Language> centralEuropean = {
        QLocale::Slovenian, QLocale::Croatian, QLocale::Czech,    QLocale::Slovak, QLocale::Polish,
        QLocale::Hungarian, QLocale::Romanian, QLocale::Albanian, QLocale::Bosnian};
    QStringDecoder legacy(centralEuropean.contains(language) ? "windows-1250" : "windows-1252");
    if (!legacy.isValid())
        legacy = QStringDecoder(QStringConverter::Latin1);
    return legacy.decode(bytes);
}

} // namespace noteAppC
