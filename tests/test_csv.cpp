// The CSV/TSV parser behind `![[data.csv]]` (libs/editor/CSVparser.h).

#include <gtest/gtest.h>

#include "CSVparser.h"

using namespace noteAppC;

namespace {

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
