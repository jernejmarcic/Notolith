#pragma once

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QLocale>
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
    int columns = 0;       ///< the widest kept record, at most `maxColumns`
    int hiddenColumns = 0; ///< how many more cells the widest kept record had
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
///
/// What a hostile file may cost is bounded here, before anything is padded
/// or copied: a record keeps at most `maxColumns` cells (the others are
/// counted in `hiddenColumns` and never stored) and a cell at most
/// `maxCellChars` characters, an ellipsis ending one that was cut. Negative
/// means no limit.
CSVtable parseCSV(QStringView text, QChar delimiter, qsizetype maxRows = -1, int maxColumns = -1,
                  int maxCellChars = -1);

/// A file's bytes as text: by byte order mark when it has one (UTF-8, 16, 32),
/// else UTF-8, and when the bytes are not UTF-8 - invalid, and without one
/// well-formed multibyte sequence, so a UTF-8 file with a stray bad byte stays
/// UTF-8 - the legacy code page Excel wrote it in: Windows-1250 for a Central
/// European `language`, otherwise Windows-1252 (Latin-1 where Qt has no such
/// codec).
QString decodeCSV(const QByteArray &bytes, QLocale::Language language = QLocale::system().language());

} // namespace noteAppC
