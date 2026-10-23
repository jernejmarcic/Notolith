// The CSV/TSV parser behind `![[data.csv]]` (libs/editor/CSVparser.h).

#include <gtest/gtest.h>

#include <QLocale>
#include <QStringConverter>

#include <malloc.h>

#include "CSVparser.h"

using namespace noteAppC;

// Live heap bytes, from glibc: Qt's string and list buffers come from malloc
// itself (QArrayData), so replacing operator new would not see them. A test
// takes this before and after the parse, the result still alive, and what is
// left is what the parse kept.
namespace {
long long heapInUse()
{
    const struct mallinfo2 info = mallinfo2();
    return static_cast<long long>(info.uordblks) + static_cast<long long>(info.hblkhd);
}

QList<QStringList> rowsOf(const QString &text, QChar delimiter = QLatin1Char(','))
{
    return parseCSV(text, delimiter).rows;
}

QStringList row(std::initializer_list<const char *> cells)
{
    QStringList list;
    for (const char *cell : cells)
        list << QString::fromUtf8(cell);
    return list;
}

} // namespace

TEST(CSVparserTest, PlainRows)
{
    const auto rows = rowsOf(QStringLiteral("a,b,c\n1,2,3\n"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0), row({"a", "b", "c"}));
    EXPECT_EQ(rows.at(1), row({"1", "2", "3"}));
}

TEST(CSVparserTest, NoTrailingNewlineAndEmptyCells)
{
    const auto rows = rowsOf(QStringLiteral("a,,c\n,,"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0), row({"a", "", "c"}));
    EXPECT_EQ(rows.at(1), row({"", "", ""}));
}

TEST(CSVparserTest, QuotedFieldsHoldDelimitersAndEscapedQuotes)
{
    const auto rows = rowsOf(QStringLiteral("name,note\n\"Smith, J\",\"say \"\"hi\"\"\"\n"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(1), row({"Smith, J", "say \"hi\""}));
}

TEST(CSVparserTest, QuotedFieldsHoldLineBreaks)
{
    const auto rows = rowsOf(QStringLiteral("a,b\n\"line1\nline2\",x\n\"c\r\nd\",y\n"));
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows.at(1), row({"line1\nline2", "x"}));
    EXPECT_EQ(rows.at(2), row({"c\r\nd", "y"}));
}

TEST(CSVparserTest, CRLFAndLoneCRSeparateRecords)
{
    EXPECT_EQ(rowsOf(QStringLiteral("a,b\r\n1,2\r\n")).size(), 2);
    EXPECT_EQ(rowsOf(QStringLiteral("a,b\r1,2\r")).size(), 2);
    EXPECT_EQ(rowsOf(QStringLiteral("a,b\r\n1,2\r\n")).at(1), row({"1", "2"}));
}

TEST(CSVparserTest, ByteOrderMarkIsNotPartOfTheFirstCell)
{
    const auto rows = rowsOf(QString(QChar(0xFEFF)) + QStringLiteral("id,name\n1,x\n"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0), row({"id", "name"}));
}

TEST(CSVparserTest, RaggedRowsPadToTheWidestRecord)
{
    const CSVtable table = parseCSV(QStringLiteral("a,b,c\n1\n1,2,3,4\n"), QLatin1Char(','));
    EXPECT_EQ(table.columns, 4);
    ASSERT_EQ(table.rows.size(), 3);
    for (const QStringList &record : table.rows)
        EXPECT_EQ(record.size(), 4);
    EXPECT_EQ(table.rows.at(1), row({"1", "", "", ""}));
    EXPECT_EQ(table.rows.at(0), row({"a", "b", "c", ""}));
}

TEST(CSVparserTest, EmptyAndBlankInputHaveNoRows)
{
    for (const QString &text : {QString(), QStringLiteral("\n"), QStringLiteral("\r\n\r\n"),
                                QString(QChar(0xFEFF))}) {
        const CSVtable table = parseCSV(text, QLatin1Char(','));
        EXPECT_TRUE(table.rows.isEmpty());
        EXPECT_EQ(table.totalRows, 0);
        EXPECT_EQ(table.columns, 0);
    }
}

TEST(CSVparserTest, BlankLinesBetweenRecordsAreSkipped)
{
    EXPECT_EQ(rowsOf(QStringLiteral("a,b\n\n1,2\n")).size(), 2);
}

TEST(CSVparserTest, AQuoteInsideAnUnquotedFieldIsLiteral)
{
    const auto rows = rowsOf(QStringLiteral("5\" pipe,ok\n"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0), row({"5\" pipe", "ok"}));
}

TEST(CSVparserTest, AnUnterminatedQuoteTakesTheRestOfTheText)
{
    const auto rows = rowsOf(QStringLiteral("a,\"open\nstill open"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0), row({"a", "open\nstill open"}));
}

TEST(CSVparserTest, TabAndSemicolonDelimiters)
{
    EXPECT_EQ(rowsOf(QStringLiteral("a\tb\n1\t2\n"), QLatin1Char('\t')).at(1), row({"1", "2"}));
    EXPECT_EQ(rowsOf(QStringLiteral("a;b\n1,5;2\n"), QLatin1Char(';')).at(1), row({"1,5", "2"}));
}

TEST(CSVparserTest, SniffsTheDelimiterFromTheFirstLine)
{
    EXPECT_EQ(CSVdelimiter(QStringLiteral("a,b,c\n1;2;3\n")), QLatin1Char(','));
    EXPECT_EQ(CSVdelimiter(QStringLiteral("a;b;c\n1,5,2,5,3\n")), QLatin1Char(';'));
    EXPECT_EQ(CSVdelimiter(QStringLiteral("a\tb\tc\n")), QLatin1Char('\t'));
    EXPECT_EQ(CSVdelimiter(QStringLiteral("single\nrow\n")), QLatin1Char(','));
    EXPECT_EQ(CSVdelimiter(QString()), QLatin1Char(','));
    // a delimiter inside quotes does not count; and a BOM is skipped
    EXPECT_EQ(CSVdelimiter(QStringLiteral("\"a,b,c,d\";e;f\n")), QLatin1Char(';'));
    EXPECT_EQ(CSVdelimiter(QString(QChar(0xFEFF)) + QStringLiteral("a;b\n")), QLatin1Char(';'));
}

TEST(CSVparserTest, MaxRowsKeepsTheHeadAndCountsTheRest)
{
    QString text = QStringLiteral("h1,h2\n");
    for (int i = 0; i < 10; ++i)
        text += QStringLiteral("%1,x\n").arg(i);
    const CSVtable table = parseCSV(text, QLatin1Char(','), 4);
    EXPECT_EQ(table.rows.size(), 4);
    EXPECT_EQ(table.totalRows, 11);
    EXPECT_EQ(table.rows.at(3).at(0), QStringLiteral("2"));
}

TEST(CSVparserTest, ALineOfOnlyAnEmptyQuotedFieldIsARecord)
{
    const auto rows = rowsOf(QStringLiteral("a\n\"\"\nb\n"));
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows.at(1), row({""}));
}

TEST(CSVparserTest, QuotesAfterAClosedQuoteAreLiteral)
{
    const auto rows = rowsOf(QStringLiteral("\"a\"b\"c,d\n"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0), row({"ab\"c", "d"}));
}

TEST(CSVparserTest, AByteOrderMarkBeforeAQuotedFirstField)
{
    const auto rows = rowsOf(QString(QChar(0xFEFF)) + QStringLiteral("\"a,b\",c\n"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0), row({"a,b", "c"}));
}

TEST(CSVparserTest, TheSnifferReadsQuotesTheWayTheParserDoes)
{
    // an inch mark in the middle of a header is not a quote, so the ';' count stands
    EXPECT_EQ(CSVdelimiter(QStringLiteral("Size 5\";Price;Qty\n1;2;3\n")), QLatin1Char(';'));
    EXPECT_EQ(CSVdelimiter(QStringLiteral("\"a;b\",c,d\n")), QLatin1Char(','));
}

TEST(CSVparserTest, MaxColumnsKeepsTheLeftAndCountsTheRest)
{
    const CSVtable table = parseCSV(QStringLiteral("a,b,c,d,e\n1,2\n1,2,3,4,5,6\n"), QLatin1Char(','), -1, 3);
    EXPECT_EQ(table.columns, 3);
    EXPECT_EQ(table.hiddenColumns, 3) << "the widest record had 6";
    ASSERT_EQ(table.rows.size(), 3);
    EXPECT_EQ(table.rows.at(0), row({"a", "b", "c"}));
    EXPECT_EQ(table.rows.at(1), row({"1", "2", ""}));
}

TEST(CSVparserTest, AQuoteInADroppedColumnStillGroupsTheRecord)
{
    const CSVtable table = parseCSV(QStringLiteral("a,b,\"x\ny, z\",\"q\"\"\"\n1,2,3,4\n"), QLatin1Char(','), -1, 2);
    ASSERT_EQ(table.rows.size(), 2) << "the newline inside the dropped quoted cell is not a record break";
    EXPECT_EQ(table.rows.at(1), row({"1", "2"}));
}

TEST(CSVparserTest, MaxCellCharsCutsALongCellWithAnEllipsis)
{
    const CSVtable table = parseCSV(QStringLiteral("abcdefghij,xy\n"), QLatin1Char(','), -1, -1, 4);
    ASSERT_EQ(table.rows.size(), 1);
    EXPECT_EQ(table.rows.at(0), (QStringList{QStringLiteral("abcd") + QChar(0x2026), QStringLiteral("xy")}));
}

TEST(CSVparserTest, AHugeLineOfCommasCostsAlmostNothing)
{
    // a 1 MB line of delimiters: a million empty cells, which must not become a million strings
    QString text(1 << 20, QLatin1Char(','));
    text += QStringLiteral("\n1,2\n");
    const long long base = heapInUse();
    const CSVtable table = parseCSV(text, QLatin1Char(','), 501, 64, 1000);
    EXPECT_LT(heapInUse() - base, 256 * 1024) << "bytes the parse kept (uncapped: 67 MB)";
    EXPECT_EQ(table.columns, 64);
    EXPECT_EQ(table.hiddenColumns, (1 << 20) + 1 - 64);
    EXPECT_EQ(table.rows.size(), 2);
}

TEST(CSVparserTest, AHundredThousandColumnsTimesFiveHundredRowsStaysSmall)
{
    QString text(100000, QLatin1Char(','));
    text += QLatin1Char('\n');
    for (int i = 0; i < 500; ++i)
        text += QStringLiteral("%1,x\n").arg(i);
    const long long base = heapInUse();
    const CSVtable table = parseCSV(text, QLatin1Char(','), 501, 64, 1000);
    EXPECT_LT(heapInUse() - base, 4 * 1024 * 1024) << "bytes the parse kept (uncapped: 2.1 GB)";
    EXPECT_EQ(table.rows.size(), 501);
    for (const QStringList &record : table.rows)
        ASSERT_EQ(record.size(), 64);
}

TEST(CSVparserTest, AMegabyteCellIsCutBeforeItIsKept)
{
    QString text = QStringLiteral("\"") + QString(1 << 20, QLatin1Char('x')) + QStringLiteral("\",b\n");
    const long long base = heapInUse();
    const CSVtable table = parseCSV(text, QLatin1Char(','), 501, 64, 1000);
    EXPECT_LT(heapInUse() - base, 64 * 1024) << "bytes the parse kept (uncapped: 4 MB)";
    ASSERT_EQ(table.rows.size(), 1);
    EXPECT_EQ(table.rows.at(0).at(0).size(), 1001);
    EXPECT_EQ(table.rows.at(0).at(1), QStringLiteral("b"));
}

TEST(CSVdecodeTest, UTF8WithAndWithoutABOM)
{
    EXPECT_EQ(decodeCSV("a;\xC4\x8D\n", QLocale::English), QString::fromUtf8("a;\xC4\x8D\n"));
    EXPECT_EQ(decodeCSV("\xEF\xBB\xBF" "id\n", QLocale::English).remove(QChar(0xFEFF)), QStringLiteral("id\n"));
}

TEST(CSVdecodeTest, UTF16ByByteOrderMark)
{
    const QByteArray little("\xFF\xFE" "a\0\t\0" "\x0D\x01\n\0", 10); // "a<TAB>" U+010D "\n"
    EXPECT_EQ(decodeCSV(little, QLocale::English).remove(QChar(0xFEFF)),
              QStringLiteral("a\t") + QChar(0x010D) + QStringLiteral("\n"));
    const QByteArray big("\xFE\xFF" "\0a\0\t\x01\x0D\0\n", 10);
    EXPECT_EQ(decodeCSV(big, QLocale::English).remove(QChar(0xFEFF)),
              QStringLiteral("a\t") + QChar(0x010D) + QStringLiteral("\n"));
}

TEST(CSVdecodeTest, InvalidUTF8FallsBackToAWindowsCodePage)
{
    // 0xE9 is "e acute" in Latin-1, Windows-1252 and Windows-1250 alike
    EXPECT_EQ(decodeCSV("Jo\xE9;1\n", QLocale::English), QString::fromUtf8("Jo\xC3\xA9;1\n"));
    EXPECT_EQ(decodeCSV("Jo\xE9;1\n", QLocale::Slovenian), QString::fromUtf8("Jo\xC3\xA9;1\n"));
}

TEST(CSVdecodeTest, ACentralEuropeanLocaleReadsWindows1250)
{
    if (!QStringDecoder("windows-1250").isValid())
        GTEST_SKIP() << "this Qt has no windows-1250 codec";
    // 0xE8 0x9A 0x9E: c-caron, s-caron, z-caron in 1250 (Latin-1 would give e grave and controls)
    EXPECT_EQ(decodeCSV("\xE8" "e\x9A" "e\x9E", QLocale::Slovenian),
              QString(QChar(0x010D)) + QLatin1Char('e') + QChar(0x0161) + QLatin1Char('e') + QChar(0x017E));
}

TEST(CSVparserTest, TheSnifferSkipsAnEscapedQuoteInsideAQuotedField)
{
    // the ';'s are inside the quoted field, past a "" escape; outside it there is one ','
    EXPECT_EQ(CSVdelimiter(QStringLiteral("\"a\"\"b;c;d;e\",x\n")), QLatin1Char(','));
}

TEST(CSVdecodeTest, AUTF8FileWithAStrayBadByteStaysUTF8)
{
    const QString text = decodeCSV("\xC4\x8D" "aj;1\xFF\n", QLocale::Slovenian);
    EXPECT_EQ(text, QString(QChar(0x010D)) + QStringLiteral("aj;1") + QChar(0xFFFD) + QLatin1Char('\n'));
}

TEST(CSVdecodeTest, ACodePageFileWithNoMultibyteSequenceIsStillACodePageFile)
{
    EXPECT_EQ(decodeCSV("Jo\xE9 \xE9;1\n", QLocale::English), QString::fromUtf8("Jo\xC3\xA9 \xC3\xA9;1\n"));
}

TEST(CSVparserTest, ACutNeverLeavesAHalfOfASurrogatePair)
{
    // 999 letters, then a character outside the BMP: the cap falls between its two UTF-16 units
    const QString text = QString(999, QLatin1Char('a')) + QString::fromUcs4(U"\U0001F600", 1) + QStringLiteral(",b\n");
    const CSVtable table = parseCSV(text, QLatin1Char(','), -1, -1, 1000);
    const QString cell = table.rows.at(0).at(0);
    EXPECT_EQ(cell.size(), 1000);
    EXPECT_TRUE(cell.endsWith(QChar(0x2026)));
    EXPECT_FALSE(cell.at(cell.size() - 2).isHighSurrogate());
}
