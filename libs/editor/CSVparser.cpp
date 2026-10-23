#include "CSVparser.h"

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
    for (const QChar c : text) {
        if (c == QLatin1Char('"'))
            quoted = !quoted;
        else if (quoted)
            continue;
        else if (c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            break;
        else if (c == QLatin1Char(','))
            ++commas;
        else if (c == QLatin1Char(';'))
            ++semicolons;
        else if (c == QLatin1Char('\t'))
            ++tabs;
    }
    if (semicolons > commas && semicolons >= tabs)
        return QLatin1Char(';');
    if (tabs > commas && tabs > semicolons)
        return QLatin1Char('\t');
    return QLatin1Char(',');
}

CSVtable parseCSV(QStringView text, QChar delimiter, qsizetype maxRows)
{
    text = withoutBOM(text);
    CSVtable table;
    QStringList record;
    QString field;
    bool quoted = false;     // inside a quoted field
    bool fieldQuoted = false; // the field being read opened with a quote
    bool pending = false;    // the record has any content at all, so a blank line is not one

    const auto endField = [&] {
        record << field;
        field.clear();
        fieldQuoted = false;
    };
    const auto endRecord = [&] {
        if (pending) {
            endField();
            ++table.totalRows;
            if (maxRows < 0 || table.rows.size() < maxRows) {
                table.columns = std::max(table.columns, int(record.size()));
                table.rows << record;
            }
        }
        record.clear();
        field.clear();
        fieldQuoted = false;
        pending = false;
    };

    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c != QLatin1Char('"'))
                field += c;
            else if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                field += c;
                ++i;
            } else
                quoted = false;
        } else if (c == QLatin1Char('"') && field.isEmpty() && !fieldQuoted) {
            quoted = fieldQuoted = pending = true;
        } else if (c == delimiter) {
            endField();
            pending = true;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;
            endRecord();
        } else {
            field += c;
            pending = true;
        }
    }
    endRecord();

    for (QStringList &padded : table.rows) {
        while (padded.size() < table.columns)
            padded << QString();
    }
    return table;
}

} // namespace noteAppC
