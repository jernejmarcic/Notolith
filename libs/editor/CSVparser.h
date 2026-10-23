#pragma once

#include <QChar>
#include <QList>
#include <QStringList>
#include <QStringView>

namespace noteAppC {

/// A parsed CSV/TSV file: what `![[data.csv]]` draws as a table.
struct CSVtable {
    /// The first `maxRows` records, the header first, each padded with empty
    /// cells to `columns` so a ragged file is still a grid.
    QList<QStringList> rows;
    /// Every record the text holds, shown or not: the "N more rows" footer.
    qsizetype totalRows = 0;
    int columns = 0; ///< the widest record among `rows`
};

/// The delimiter a `.csv` uses, from its first line: whichever of `,`, `;`
/// and tab occurs most outside quotes (European Excel writes `;`). A tie, or
/// none at all, is a comma.
QChar CSVdelimiter(QStringView text);

/// RFC 4180: quoted fields, `""` for a quote inside one, delimiters and line
/// breaks (LF, CRLF or CR) inside quotes, a leading UTF-8 BOM ignored. A
/// quote only opens a field at its start; anywhere else it is literal. A
/// blank line is not a record. `maxRows` < 0 keeps every record; otherwise
/// the rest are counted but not kept.
CSVtable parseCSV(QStringView text, QChar delimiter, qsizetype maxRows = -1);

} // namespace noteAppC
