#include <gtest/gtest.h>

#include <QTableView>
#include <QHeaderView>

#include <QComboBox>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QApplication>

#include "BaseEditorTable.h"
#include "BaseFile.h"
#include "BaseQuery.h"
#include "BaseTableModel.h"
#include "BaseView.h"
#include "Expression.h"
#include "Index.h"
#include "Indexer.h"
#include "Vault.h"
#include "WaitFor.h"
#include "Perf.h"

using namespace noteAppC;
using namespace noteAppC::bases;

namespace {

Row makeRow(const QString &name, const QString &folder = QString())
{
    Row row;
    row.noteName = name;
    row.notePath = folder.isEmpty() ? name + QStringLiteral(".md")
                                    : folder + QLatin1Char('/') + name + QStringLiteral(".md");
    row.noteFolder = folder;
    row.noteExt = QStringLiteral("md");
    row.fileMtime = QDateTime(QDate(2026, 1, 1), QTime(12, 0));
    return row;
}

Value evalText(const QString &source, const Row &row, const FormulaSource *formulas = nullptr,
              QString *error = nullptr)
{
    QString parseError;
    auto expr = Expression::parse(source, &parseError);
    if (!expr) {
        if (error) *error = parseError;
        return Value::empty();
    }
    QString evalError;
    Value v = expr->evaluate(row, formulas, QDateTime(QDate(2026, 9, 23), QTime(12, 0)), &evalError);
    if (error) *error = evalError;
    return v;
}

} // namespace

// =========================================================================
// Expression: literals, property access, operators
// =========================================================================

TEST(ExpressionTest, Literals)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("2.5"), row).number, 2.5);
    EXPECT_EQ(evalText(QStringLiteral("\"hi\""), row).text, QStringLiteral("hi"));
    EXPECT_EQ(evalText(QStringLiteral("'hi'"), row).text, QStringLiteral("hi"));
    EXPECT_TRUE(evalText(QStringLiteral("true"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("false"), row).boolean);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(2.5)"), row).number, 2.5);
}

TEST(ExpressionTest, FilePropertyAccess)
{
    Row row = makeRow(QStringLiteral("Home"), QStringLiteral("Projects"));
    row.fileSize = 1234;
    row.tags = {QStringLiteral("a"), QStringLiteral("b")};
    row.links = {QStringLiteral("Other")};

    EXPECT_EQ(evalText(QStringLiteral("file.name"), row).text, QStringLiteral("Home"));
    EXPECT_EQ(evalText(QStringLiteral("file.folder"), row).text, QStringLiteral("Projects"));
    EXPECT_EQ(evalText(QStringLiteral("file.ext"), row).text, QStringLiteral("md"));
    EXPECT_EQ(evalText(QStringLiteral("file.path"), row).text, QStringLiteral("Projects/Home.md"));
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("file.size"), row).number, 1234);
    EXPECT_EQ(evalText(QStringLiteral("file.tags"), row).strings(), QStringList({"a", "b"}));
    EXPECT_EQ(evalText(QStringLiteral("file.links"), row).strings(), QStringList({"Other"}));
    EXPECT_EQ(evalText(QStringLiteral("file.tags.length"), row).number, 2);
}

TEST(ExpressionTest, NotePropertyBareAndDottedAndBracket)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("price"), Value::ofNumber(9.5));
    row.properties.insert(QStringLiteral("two words"), Value::ofString(QStringLiteral("x")));

    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("price"), row).number, 9.5);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("note.price"), row).number, 9.5);
    EXPECT_EQ(evalText(QStringLiteral("note[\"two words\"]"), row).text, QStringLiteral("x"));
    // Missing property: empty, not an error.
    QString error;
    EXPECT_TRUE(evalText(QStringLiteral("missing"), row, nullptr, &error).isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

TEST(ExpressionTest, ArithmeticAndComparison)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("2 + 3 * 4"), row).number, 14);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(2 + 3) * 4"), row).number, 20);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("10 % 3"), row).number, 1);
    EXPECT_EQ(evalText(QStringLiteral("\"a\" + \"b\""), row).text, QStringLiteral("ab"));
    EXPECT_TRUE(evalText(QStringLiteral("3 > 2"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("2 <= 2"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("\"a\" == \"a\""), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("\"a\" != \"b\""), row).boolean);

    QString error;
    evalText(QStringLiteral("1 / 0"), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty());
}

TEST(ExpressionTest, BooleanOperatorsShortCircuit)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_TRUE(evalText(QStringLiteral("true || (1/0 > 0)"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("false && (1/0 > 0)"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("!false"), row).boolean);
}

TEST(ExpressionTest, DateArithmeticAndComparison)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("start"), Value::ofDate(QDateTime(QDate(2026, 1, 1), QTime(0, 0))));

    const Value plus = evalText(QStringLiteral("start + \"2w\""), row);
    ASSERT_EQ(plus.type, Value::Type::Date);
    EXPECT_EQ(plus.date.date(), QDate(2026, 1, 15));

    const Value minusMonth = evalText(QStringLiteral("start - \"1M\""), row);
    EXPECT_EQ(minusMonth.date.date(), QDate(2025, 12, 1));

    EXPECT_TRUE(evalText(QStringLiteral("now() > start"), row).boolean);

    QString error;
    // The worded duration form, straight from the docs' own filter example.
    evalText(QStringLiteral("file.mtime > now() - \"1 week\""), row, nullptr, &error);
    EXPECT_TRUE(error.isEmpty());
    evalText(QStringLiteral("start + \"2w\""), row, nullptr, &error);
    EXPECT_TRUE(error.isEmpty());
}

TEST(ExpressionTest, GlobalFunctions)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("price"), Value::ofNumber(0));
    EXPECT_TRUE(evalText(QStringLiteral("if(1 > 0, \"yes\", \"no\")"), row).text == QStringLiteral("yes"));
    EXPECT_TRUE(evalText(QStringLiteral("if(price, \"has\")"), row).isEmpty()); // price is 0, falsy
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("min(3, 1, 2)"), row).number, 1);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("max(3, 1, 2)"), row).number, 3);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("number(\"42\")"), row).number, 42);
    EXPECT_EQ(evalText(QStringLiteral("today()"), row).date.time(), QTime(0, 0));
    EXPECT_EQ(evalText(QStringLiteral("date(\"2026-01-01\")"), row).date.date(), QDate(2026, 1, 1));
}

TEST(ExpressionTest, AnyTypeFunctions)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_TRUE(evalText(QStringLiteral("(1).isTruthy()"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("(0).isTruthy()"), row).boolean);
    EXPECT_EQ(evalText(QStringLiteral("(42).toString()"), row).text, QStringLiteral("42"));
    EXPECT_TRUE(evalText(QStringLiteral("(42).isType(\"number\")"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("(42).isType(\"string\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("\"\".isEmpty()"), row).boolean);
}

TEST(ExpressionTest, StringFunctions)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("s"), Value::ofString(QStringLiteral("Hello World")));
    EXPECT_TRUE(evalText(QStringLiteral("s.contains(\"World\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("s.startsWith(\"Hello\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("s.endsWith(\"World\")"), row).boolean);
    EXPECT_EQ(evalText(QStringLiteral("s.lower()"), row).text, QStringLiteral("hello world"));
    EXPECT_EQ(evalText(QStringLiteral("s.title()"), row).text, QStringLiteral("Hello World"));
    EXPECT_EQ(evalText(QStringLiteral("s.trim()"), row).text, QStringLiteral("Hello World"));
    EXPECT_EQ(evalText(QStringLiteral("s.replace(\"World\", \"There\")"), row).text,
             QStringLiteral("Hello There"));
    EXPECT_EQ(evalText(QStringLiteral("s.slice(0, 5)"), row).text, QStringLiteral("Hello"));
    EXPECT_EQ(evalText(QStringLiteral("s.slice(-5)"), row).text, QStringLiteral("World"));
    EXPECT_EQ(evalText(QStringLiteral("s.split(\" \")"), row).strings(), QStringList({"Hello", "World"}));
    EXPECT_EQ(evalText(QStringLiteral("\"ab\".repeat(3)"), row).text, QStringLiteral("ababab"));
    EXPECT_EQ(evalText(QStringLiteral("\"abc\".reverse()"), row).text, QStringLiteral("cba"));
    EXPECT_TRUE(evalText(QStringLiteral("s.containsAny(\"xx\", \"World\")"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("s.containsAll(\"Hello\", \"xx\")"), row).boolean);
}

TEST(ExpressionTest, NumberFunctions)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(-3.7).abs()"), row).number, 3.7);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(2.2).ceil()"), row).number, 3);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(2.8).floor()"), row).number, 2);
    EXPECT_DOUBLE_EQ(evalText(QStringLiteral("(2.567).round(2)"), row).number, 2.57);
    EXPECT_EQ(evalText(QStringLiteral("(2).toFixed(2)"), row).text, QStringLiteral("2.00"));
}

TEST(ExpressionTest, FilterMapAndReduceBindValueIndexAndAcc)
{
    const Row row = makeRow(QStringLiteral("N"));
    // Obsidian's own examples, from its functions reference.
    EXPECT_EQ(evalText(QStringLiteral("[1,2,3,4].filter(value > 2)"), row).strings(), QStringList({"3", "4"}));
    EXPECT_EQ(evalText(QStringLiteral("[1,2,3,4].map(value + 1)"), row).strings(), QStringList({"2", "3", "4", "5"}));
    const Value sum = evalText(QStringLiteral("[1,2,3].reduce(acc + value, 0)"), row);
    EXPECT_EQ(sum.type, Value::Type::Number) << "a sum, not the digits joined";
    EXPECT_EQ(sum.number, 6);
    EXPECT_EQ(evalText(QStringLiteral("[\"a\",\"b\",\"c\"].map(index)"), row).strings(), QStringList({"0", "1", "2"}));
    // Nested: the inner call's value is its own, and the outer's comes back.
    EXPECT_EQ(evalText(QStringLiteral("[[1,2],[3]].map(value.map(value * 10).join(\"+\"))"), row).strings(),
              QStringList({"10+20", "30"}));
    EXPECT_EQ(evalText(QStringLiteral("[[1,2],[3]].flat()"), row).strings(), QStringList({"1", "2", "3"}));
    EXPECT_EQ(evalText(QStringLiteral("[4,1,7].mean()"), row).number, 4);
    QString error;
    evalText(QStringLiteral("[1].reduce(acc + value)"), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty()) << "reduce needs its initial value";
}

TEST(ExpressionTest, ListFunctions)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("tags"), Value::ofList({"a", "b", "a", "c"}));
    EXPECT_TRUE(evalText(QStringLiteral("tags.contains(\"b\")"), row).boolean);
    EXPECT_EQ(evalText(QStringLiteral("tags.join(\",\")"), row).text, QStringLiteral("a,b,a,c"));
    EXPECT_EQ(evalText(QStringLiteral("tags.unique()"), row).strings(), QStringList({"a", "b", "c"}));
    // Reversal on a non-palindromic list actually proves the order flipped.
    EXPECT_EQ(evalText(QStringLiteral("tags.reverse()"), row).strings(), QStringList({"c", "a", "b", "a"}));
    EXPECT_EQ(evalText(QStringLiteral("tags.sort()"), row).strings(), QStringList({"a", "a", "b", "c"}));
    EXPECT_EQ(evalText(QStringLiteral("tags.length"), row).number, 4);
    EXPECT_FALSE(evalText(QStringLiteral("tags.isEmpty()"), row).boolean);
}

TEST(ExpressionTest, FileFunctions)
{
    Row row = makeRow(QStringLiteral("N"), QStringLiteral("Books/SciFi"));
    row.tags = {QStringLiteral("book"), QStringLiteral("read")};
    row.links = {QStringLiteral("Textbook"), QStringLiteral("Other/Note")};
    row.properties.insert(QStringLiteral("title"), Value::ofString(QStringLiteral("Dune")));

    EXPECT_TRUE(evalText(QStringLiteral("file.hasTag(\"book\")"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("file.hasTag(\"movie\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("file.inFolder(\"Books\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("file.inFolder(\"Books/SciFi\")"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("file.inFolder(\"Movies\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("file.hasLink(\"Textbook\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("file.hasLink(\"Note\")"), row).boolean); // basename match
    EXPECT_FALSE(evalText(QStringLiteral("file.hasLink(\"Nope\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("file.hasProperty(\"title\")"), row).boolean);
    EXPECT_FALSE(evalText(QStringLiteral("file.hasProperty(\"nope\")"), row).boolean);
}

// =========================================================================
// Link values: [[wikilinks]], file.links, link(), file.asLink(), this,
// ==/!=, .asFile(), .linksTo() - obsidian.md/help/bases/functions and
// /syntax (see Expression.h's own doc comment for exact quotes).
// =========================================================================

TEST(ExpressionTest, LinkValuesDisplayAndType)
{
    Row row = makeRow(QStringLiteral("Home"), QStringLiteral("Projects"));
    row.links = {QStringLiteral("Other"), QStringLiteral("Books/Textbook")};

    // file.links: still the raw target text un-aliased (Value::toDisplayString
    // falls back to the target when there is no explicit alias) - so this
    // stays exactly what ExpressionTest.FilePropertyAccess already checks,
    // now backed by Link values rather than plain strings.
    EXPECT_EQ(evalText(QStringLiteral("file.links"), row).strings(), QStringList({"Other", "Books/Textbook"}));
    EXPECT_EQ(evalText(QStringLiteral("file.links.map(value.isType(\"link\")).join(\",\")"), row).text,
             QStringLiteral("true,true"));

    // link(path, display?)
    EXPECT_EQ(evalText(QStringLiteral("link(\"Target\")"), row).toDisplayString(), QStringLiteral("Target"));
    EXPECT_EQ(evalText(QStringLiteral("link(\"Target\", \"Alias\")"), row).toDisplayString(), QStringLiteral("Alias"));
    EXPECT_TRUE(evalText(QStringLiteral("link(\"x\").isType(\"link\")"), row).boolean);

    // file.asLink(display?): defaults to the note's own name, not its path.
    EXPECT_EQ(evalText(QStringLiteral("file.asLink()"), row).toDisplayString(), QStringLiteral("Home"));
    EXPECT_EQ(evalText(QStringLiteral("file.asLink(\"Custom\")"), row).toDisplayString(), QStringLiteral("Custom"));

    // this: a Link to the row's own file.
    EXPECT_EQ(evalText(QStringLiteral("this"), row).toDisplayString(), QStringLiteral("Home"));
    EXPECT_TRUE(evalText(QStringLiteral("this == file.asLink()"), row).boolean);
}

TEST(ExpressionTest, LinkEqualityComparesByFileNotText)
{
    Row row = makeRow(QStringLiteral("N"));
    // "Textbook" (bare) and "Books/Textbook" (full path) name the same file -
    // the same leniency file.hasLink() already gives a bare wikilink target.
    EXPECT_TRUE(evalText(QStringLiteral("link(\"Textbook\") == link(\"Books/Textbook\")"), row).boolean);
    EXPECT_TRUE(evalText(QStringLiteral("link(\"A\") != link(\"B\")"), row).boolean);
    // Neither resolves against anything here, so this is a text comparison -
    // still equal because the raw text matches.
    EXPECT_TRUE(evalText(QStringLiteral("link(\"Same\") == link(\"same\")"), row).boolean); // case-folded
    // An alias (what a [[Note|Alias]] property becomes - see
    // BaseQueryRowTest.WikilinkPropertiesBecomeLinkValues for the parsing
    // itself) never affects equality, only display.
    row.properties.insert(QStringLiteral("author"), Value::ofLink(QStringLiteral("Textbook"), QStringLiteral("My Book")));
    EXPECT_EQ(evalText(QStringLiteral("author"), row).toDisplayString(), QStringLiteral("My Book"));
    EXPECT_TRUE(evalText(QStringLiteral("author == link(\"Textbook\")"), row).boolean);
}

TEST(ExpressionTest, AsFileIsIdentityAndUnknownLinkMethodFails)
{
    Row row = makeRow(QStringLiteral("N"));
    EXPECT_TRUE(evalText(QStringLiteral("link(\"x\").asFile() == link(\"x\")"), row).boolean);
    QString error;
    evalText(QStringLiteral("link(\"x\").nope()"), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty());
}

TEST(ExpressionTest, LinksToWithNoFormulaSourceIsFalseNotACrash)
{
    // BaseQueryTest.LinksToResolvesThroughTheQuerysOwnRowsNotJustTheCurrentOne
    // covers a real resolver; a bare Expression (no BaseQuery, formulas ==
    // nullptr) has
    // none, and linksTo() must still answer rather than fail the expression.
    Row row = makeRow(QStringLiteral("N"));
    QString error;
    const Value v = evalText(QStringLiteral("link(\"x\").linksTo(\"y\")"), row, nullptr, &error);
    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(v.boolean);
}

TEST(ExpressionTest, LinkListContainsComparesByIdentityNotDisplayText)
{
    Row row = makeRow(QStringLiteral("Author"), QStringLiteral("People"));
    row.properties.insert(QStringLiteral("authors"),
                          Value::ofValues({Value::ofLink(QStringLiteral("People/Author"), QStringLiteral("A. N. Other"))}));
    // The alias "A. N. Other" is not what "this" (People/Author.md) equals
    // as text, but the two Links resolve to the same file.
    EXPECT_TRUE(evalText(QStringLiteral("authors.contains(this)"), row).boolean);
    // Plain lists keep working exactly as before (ExpressionTest.ListFunctions
    // already covers same-type; this is the cross-type "2" vs 2 leniency).
    Row plain = makeRow(QStringLiteral("N"));
    plain.properties.insert(QStringLiteral("nums"), Value::ofValues({Value::ofNumber(2), Value::ofNumber(3)}));
    EXPECT_TRUE(evalText(QStringLiteral("nums.contains(\"2\")"), plain).boolean);
}

TEST(ExpressionTest, UnsupportedGivesAClearErrorNeverACrash)
{
    Row row = makeRow(QStringLiteral("N"));
    row.properties.insert(QStringLiteral("tags"), Value::ofList({"a"}));

    QString error;
    Expression::parse(QStringLiteral("(("), &error);
    EXPECT_FALSE(error.isEmpty()); // bad syntax

    evalText(QStringLiteral("tags.toBase64()"), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty()); // no such list function

    evalText(QStringLiteral("nope(1)"), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty()); // unknown global function

    evalText(QStringLiteral("1 > \"x\""), row, nullptr, &error);
    EXPECT_FALSE(error.isEmpty()); // incomparable types
}

// =========================================================================
// BaseFile: parsing the documented format
// =========================================================================

TEST(BaseFileTest, ParsesTheDocumentedExample)
{
    // Shape confirmed against https://obsidian.md/help/bases/syntax's own
    // example (a table view with a global and/or/not filter tree, formulas,
    // property display names and a groupBy).
    const QString yaml = QStringLiteral(R"YAML(
filters:
  and:
    - 'status != "done"'
formulas:
  formatted_price: 'if(price, price.toFixed(2) + " dollars")'
  ppu: "(price / age).toFixed(2)"
properties:
  status:
    displayName: Status
  formula.formatted_price:
    displayName: "Price"
summaries:
  customAverage: 'values.mean().round(3)'
views:
  - type: table
    name: "My table"
    limit: 10
    groupBy:
      property: note.age
      direction: DESC
    filters:
      and:
        - or:
            - "formula.ppu > 5"
            - "price > 2.1"
    order:
      - file.name
      - note.age
      - formula.ppu
)YAML");
    QString error;
    auto file = BaseFile::parse(yaml, &error);
    ASSERT_TRUE(file.has_value()) << error.toStdString();

    EXPECT_EQ(file->globalFilters().kind, FilterNode::Kind::And);
    ASSERT_EQ(file->globalFilters().children.size(), 1);
    EXPECT_EQ(file->globalFilters().children.first().statement, QStringLiteral("status != \"done\""));

    ASSERT_EQ(file->formulas().size(), 2);
    EXPECT_EQ(file->formulas().at(1).name, QStringLiteral("ppu"));

    EXPECT_EQ(file->displayNameFor(QStringLiteral("status")), QStringLiteral("Status"));
    EXPECT_EQ(file->displayNameFor(QStringLiteral("formula.formatted_price")), QStringLiteral("Price"));
    // No configured display name: humanized from the reference itself.
    EXPECT_EQ(file->displayNameFor(QStringLiteral("note.age")), QStringLiteral("Age"));

    ASSERT_EQ(file->views().size(), 1);
    const ViewConfig &view = file->views().first();
    EXPECT_EQ(view.type, QStringLiteral("table"));
    EXPECT_EQ(view.name, QStringLiteral("My table"));
    EXPECT_EQ(view.limit, 10);
    EXPECT_EQ(view.groupBy.property, QStringLiteral("note.age"));
    EXPECT_EQ(view.groupBy.direction, SortDirection::Descending);
    EXPECT_EQ(view.order, QStringList({"file.name", "note.age", "formula.ppu"}));
    EXPECT_EQ(view.filters.kind, FilterNode::Kind::And);
    ASSERT_EQ(view.filters.children.size(), 1);
    EXPECT_EQ(view.filters.children.first().kind, FilterNode::Kind::Or);
}

TEST(BaseFileTest, KeepsUnknownKeysOnSerialize)
{
    const QString yaml = QStringLiteral(
        "summaries:\n  customAverage: 'values.mean()'\n"
        "views:\n  - type: table\n    name: T\n    somethingFuture: 42\n");
    auto file = BaseFile::parse(yaml);
    ASSERT_TRUE(file.has_value());
    const QString out = file->serialize();
    EXPECT_TRUE(out.contains(QStringLiteral("summaries")));
    EXPECT_TRUE(out.contains(QStringLiteral("customAverage")));
    EXPECT_TRUE(out.contains(QStringLiteral("somethingFuture")));
}

TEST(BaseFileTest, ComplexOrAndAndNotTree)
{
    const QString yaml = QStringLiteral(R"YAML(
filters:
  or:
    - file.hasTag("tag")
    - and:
        - file.hasTag("book")
        - file.hasLink("Textbook")
    - not:
        - file.hasTag("book")
        - file.inFolder("Required Reading")
)YAML");
    auto file = BaseFile::parse(yaml);
    ASSERT_TRUE(file.has_value());
    const FilterNode &root = file->globalFilters();
    EXPECT_EQ(root.kind, FilterNode::Kind::Or);
    ASSERT_EQ(root.children.size(), 3);
    EXPECT_EQ(root.children.at(0).kind, FilterNode::Kind::Statement);
    EXPECT_EQ(root.children.at(1).kind, FilterNode::Kind::And);
    EXPECT_EQ(root.children.at(2).kind, FilterNode::Kind::Not);
    EXPECT_EQ(root.children.at(2).children.size(), 2);
}

TEST(BaseFileTest, InvalidYamlFails)
{
    QString error;
    auto file = BaseFile::parse(QStringLiteral("filters: [oops"), &error);
    EXPECT_FALSE(file.has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(BaseFileTest, NoViewsGetsAnImplicitTable)
{
    auto file = BaseFile::parse(QStringLiteral("formulas:\n  x: \"1\"\n"));
    ASSERT_TRUE(file.has_value());
    ASSERT_EQ(file->views().size(), 1);
    EXPECT_EQ(file->views().first().type, QStringLiteral("table"));
}

namespace {

BaseFile parseOrDie(const QString &yaml)
{
    auto file = BaseFile::parse(yaml);
    if (!file) ADD_FAILURE() << "base file failed to parse";
    return file ? std::move(*file) : BaseFile();
}

} // namespace

// =========================================================================
// BaseFile::rewriteViewSort: a header click's save-back, byte-exact -
// comments, key order and everything the click did not touch survive.
// =========================================================================

TEST(BaseFileSortRewriteTest, AppendsSortToAViewThatHasNone)
{
    const QString yaml = QStringLiteral(
        "# top comment\n"
        "views:\n"
        "  - type: table\n"
        "    name: T\n"
        "    order: [file.name, price]\n"
        "  - type: cards\n"
        "    name: C\n"
        "    order: [file.name]\n");
    const std::optional<QString> rewritten =
        BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("price"), SortDirection::Ascending);
    ASSERT_TRUE(rewritten.has_value());
    const QString &out = *rewritten;

    // Nothing before the new block moved: the whole original text up to and
    // including "order: [file.name, price]\n" is still there, byte for byte.
    const qsizetype cut = yaml.indexOf(QStringLiteral("order: [file.name, price]\n"))
                         + QStringLiteral("order: [file.name, price]\n").size();
    EXPECT_EQ(out.left(cut), yaml.left(cut));
    // The second view - everything after the insertion - is untouched too.
    EXPECT_TRUE(out.contains(QStringLiteral("  - type: cards\n    name: C\n    order: [file.name]\n")));
    EXPECT_TRUE(out.contains(QStringLiteral("    sort:\n      - property: price\n        direction: ASC\n")));

    const BaseFile reparsed = parseOrDie(out);
    ASSERT_EQ(reparsed.views().size(), 2);
    ASSERT_EQ(reparsed.views().at(0).sort.size(), 1);
    EXPECT_EQ(reparsed.views().at(0).sort.first().property, QStringLiteral("price"));
    EXPECT_EQ(reparsed.views().at(0).sort.first().direction, SortDirection::Ascending);
    EXPECT_TRUE(reparsed.views().at(1).sort.isEmpty()); // the other view: untouched
}

TEST(BaseFileSortRewriteTest, ReplacesAnExistingSortInPlace)
{
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n"
        "    order: [file.name, price] # trailing comment on order\n"
        "    sort:\n"
        "      - property: price\n"
        "        direction: ASC\n"
        "    limit: 50\n");
    const std::optional<QString> rewritten =
        BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("file.name"), SortDirection::Descending);
    ASSERT_TRUE(rewritten.has_value());
    const QString &out = *rewritten;

    // Before the sort: block, and after it (limit: 50), both untouched.
    EXPECT_TRUE(out.contains(QStringLiteral("order: [file.name, price] # trailing comment on order\n")));
    EXPECT_TRUE(out.contains(QStringLiteral("    limit: 50\n")));
    EXPECT_TRUE(out.contains(QStringLiteral("    sort:\n      - property: file.name\n        direction: DESC\n")));
    EXPECT_FALSE(out.contains(QStringLiteral("property: price")));

    const BaseFile reparsed = parseOrDie(out);
    ASSERT_EQ(reparsed.views().first().sort.size(), 1);
    EXPECT_EQ(reparsed.views().first().sort.first().property, QStringLiteral("file.name"));
    EXPECT_EQ(reparsed.views().first().sort.first().direction, SortDirection::Descending);
    // limit: survived the edit, proof the rewrite really is scoped to sort:.
    EXPECT_EQ(reparsed.views().first().limit, 50);
}

TEST(BaseFileSortRewriteTest, AViewsOwnSortAsItsVeryFirstKeyIsLeftAlone)
{
    // "sort:" sharing the sequence's own "- " line: removing "from the start
    // of this line" would eat the "- " and corrupt the views: list, so this
    // is refused rather than guessed at (see rewriteViewSort's own comment).
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - sort:\n"
        "      - property: price\n"
        "        direction: ASC\n"
        "    order: [file.name, price]\n");
    EXPECT_FALSE(BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("file.name"), SortDirection::Ascending)
                     .has_value());
}

TEST(BaseFileSortRewriteTest, AViewsFirstKeySharingTheDashLineIsFineWhenItIsNotSort)
{
    // The ordinary shape ("- type: table"): the type: key shares the dash's
    // line, but sort: itself does not, so appending after the view works.
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n"
        "    order: [file.name]\n");
    const std::optional<QString> rewritten =
        BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("file.name"), SortDirection::Ascending);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_TRUE(rewritten->startsWith(QStringLiteral("views:\n  - type: table\n    order: [file.name]\n")));
    EXPECT_TRUE(rewritten->contains(QStringLiteral("    sort:\n      - property: file.name\n")));
}

TEST(BaseFileSortRewriteTest, LastViewLastKeyAppendsAtEndOfFileEvenWithoutATrailingNewline)
{
    const QString yaml = QStringLiteral("views:\n  - type: table\n    order: [file.name]");
    ASSERT_FALSE(yaml.endsWith(QLatin1Char('\n')));
    const std::optional<QString> rewritten =
        BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("file.name"), SortDirection::Ascending);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_TRUE(rewritten->startsWith(yaml));
    const BaseFile reparsed = parseOrDie(*rewritten);
    EXPECT_EQ(reparsed.views().first().sort.first().property, QStringLiteral("file.name"));
}

TEST(BaseFileSortRewriteTest, KeyOrderAndACommentBetweenViewsSurviveUntouched)
{
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n"
        "    order: [file.name]\n"
        "  # a comment between two views\n"
        "  - type: table\n"
        "    name: Second\n"
        "    order: [file.name, price]\n");
    const std::optional<QString> rewritten =
        BaseFile::rewriteViewSort(yaml, 1, QStringLiteral("price"), SortDirection::Descending);
    ASSERT_TRUE(rewritten.has_value());
    const QString &out = *rewritten;
    EXPECT_TRUE(out.contains(QStringLiteral("  - type: table\n    order: [file.name]\n"
                                            "  # a comment between two views\n")))
        << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("name: Second")));
    EXPECT_TRUE(out.endsWith(QStringLiteral("sort:\n      - property: price\n        direction: DESC\n")));

    const BaseFile reparsed = parseOrDie(out);
    ASSERT_EQ(reparsed.views().size(), 2);
    EXPECT_TRUE(reparsed.views().at(0).sort.isEmpty());
    EXPECT_EQ(reparsed.views().at(1).sort.first().property, QStringLiteral("price"));
}

TEST(BaseFileSortRewriteTest, FlowStyleViewsAreRefusedNotCorrupted)
{
    const QString yaml = QStringLiteral("views: [{type: table, order: [file.name]}]\n");
    EXPECT_FALSE(BaseFile::rewriteViewSort(yaml, 0, QStringLiteral("file.name"), SortDirection::Ascending)
                     .has_value());
}

TEST(BaseFileSortRewriteTest, OutOfRangeViewIndexAndBadYamlAreRefused)
{
    const QString yaml = QStringLiteral("views:\n  - type: table\n    order: [file.name]\n");
    EXPECT_FALSE(BaseFile::rewriteViewSort(yaml, 5, QStringLiteral("file.name"), SortDirection::Ascending)
                     .has_value());
    EXPECT_FALSE(BaseFile::rewriteViewSort(QStringLiteral("not: [valid"), 0, QStringLiteral("file.name"),
                                           SortDirection::Ascending)
                     .has_value());
}

TEST(BaseFileSortRewriteTest, NewFileTemplateParsesToOneTableViewShowingEverything)
{
    // BaseFile::newFileTemplate() is the new-base command's starting
    // content (see MainWindow's own "canvas:new-file" for the shape the
    // lead wires it into) - it just needs to open as a working, unfiltered
    // table, the same as Obsidian's own empty base does.
    const BaseFile file = parseOrDie(BaseFile::newFileTemplate());
    ASSERT_EQ(file.views().size(), 1);
    EXPECT_EQ(file.views().first().type, QStringLiteral("table"));
    EXPECT_TRUE(file.globalFilters().matchesEverything());
}

// =========================================================================
// BaseQuery: filters, sort, limit, formulas, grouping
// =========================================================================

TEST(BaseSummaryTest, EachBuiltInSummaryOverItsKindOfColumn)
{
    const QList<Value> numbers = {Value::ofNumber(4), Value::ofNumber(1), Value::empty(), Value::ofNumber(7),
                                  Value::ofNumber(4)};
    EXPECT_EQ(summarize(QStringLiteral("Sum"), numbers), QStringLiteral("16"));
    EXPECT_EQ(summarize(QStringLiteral("Average"), numbers), QStringLiteral("4"));
    EXPECT_EQ(summarize(QStringLiteral("Min"), numbers), QStringLiteral("1"));
    EXPECT_EQ(summarize(QStringLiteral("max"), numbers), QStringLiteral("7")) << "case-insensitive";
    EXPECT_EQ(summarize(QStringLiteral("Range"), numbers), QStringLiteral("6"));
    EXPECT_EQ(summarize(QStringLiteral("Median"), numbers), QStringLiteral("4"));
    EXPECT_EQ(summarize(QStringLiteral("Stddev"), {Value::ofNumber(2), Value::ofNumber(4)}), QStringLiteral("1"));
    EXPECT_EQ(summarize(QStringLiteral("Average"), {Value::ofNumber(1), Value::ofNumber(1), Value::ofNumber(2)}),
              QStringLiteral("1.333"));
    EXPECT_EQ(summarize(QStringLiteral("Empty"), numbers), QStringLiteral("1"));
    EXPECT_EQ(summarize(QStringLiteral("Filled"), numbers), QStringLiteral("4"));
    EXPECT_EQ(summarize(QStringLiteral("Unique"), numbers), QStringLiteral("3"));

    const QDateTime first(QDate(2026, 9, 1), QTime(0, 0));
    const QList<Value> dates = {Value::ofDate(first.addDays(9)), Value::ofDate(first)};
    EXPECT_EQ(summarize(QStringLiteral("Earliest"), dates), Value::ofDate(first).toDisplayString());
    EXPECT_EQ(summarize(QStringLiteral("Latest"), dates), Value::ofDate(first.addDays(9)).toDisplayString());
    EXPECT_EQ(summarize(QStringLiteral("Range"), dates), QStringLiteral("9 days"));

    const QList<Value> checks = {Value::ofBool(true), Value::ofBool(false), Value::ofBool(true)};
    EXPECT_EQ(summarize(QStringLiteral("Checked"), checks), QStringLiteral("2"));
    EXPECT_EQ(summarize(QStringLiteral("Unchecked"), checks), QStringLiteral("1"));

    EXPECT_TRUE(summarize(QStringLiteral("customAverage"), numbers).isEmpty()) << "a file's own formula";
    EXPECT_TRUE(summarize(QStringLiteral("Sum"), {}).isEmpty());
}

TEST(BaseSummaryTest, AViewsSummariesFollowItsFilteredRows)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    order: [file.name, price]\n"
        "    filters:\n      and:\n        - 'price > 2'\n"
        "    summaries:\n      price: Sum\n"));
    QList<Row> rows;
    for (int i = 1; i <= 4; ++i) {
        Row r = makeRow(QStringLiteral("N%1").arg(i));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(i));
        rows << r;
    }
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    EXPECT_EQ(query.summaryOf(QStringLiteral("price")), QStringLiteral("7")); // 3 + 4
    EXPECT_TRUE(query.summaryOf(QStringLiteral("file.name")).isEmpty());
}

TEST(BaseSummaryTest, AFilesOwnSummaryFormulaRunsOverValues)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "summaries:\n  customAverage: 'values.mean().round(3)'\n"
        "views:\n  - type: table\n    order: [file.name, price]\n"
        "    summaries:\n      price: customAverage\n"));
    QList<Row> rows;
    for (const double price : {1.0, 1.0, 2.0}) {
        Row r = makeRow(QStringLiteral("N%1").arg(rows.size()));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(price));
        rows << r;
    }
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    EXPECT_EQ(query.summaryOf(QStringLiteral("price")), QStringLiteral("1.333"));
}

TEST(BaseQueryTest, FiltersRows)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name]\n"
        "    filters:\n      and:\n        - 'price > 5'\n"));
    QList<Row> rows;
    for (int i = 0; i < 5; ++i) {
        Row r = makeRow(QStringLiteral("N%1").arg(i));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(i * 3));
        rows << r;
    }
    BaseQuery query(file, rows);
    QString error;
    ASSERT_TRUE(query.openView(0, &error)) << error.toStdString();
    // price = 0,3,6,9,12 -> price>5 keeps 6,9,12 = 3 rows.
    EXPECT_EQ(query.resultRows().size(), 3);
    for (qsizetype idx : query.resultRows())
        EXPECT_GT(query.rows().at(idx).properties.value(QStringLiteral("price")).number, 5);
}

TEST(BaseQueryTest, InvalidFilterSyntaxFailsToOpenNotSilently)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "filters:\n  and:\n    - 'price >'\nviews:\n  - type: table\n    name: T\n"));
    QList<Row> rows{makeRow(QStringLiteral("N"))};
    BaseQuery query(file, rows);
    QString error;
    EXPECT_FALSE(query.openView(0, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(BaseQueryTest, SortsAscendingAndDescending)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name]\n"
        "    sort:\n      - property: price\n        direction: ASC\n"));
    QList<Row> rows;
    for (int v : {30, 10, 20}) {
        Row r = makeRow(QStringLiteral("N%1").arg(v));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(v));
        rows << r;
    }
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    ASSERT_EQ(query.resultRows().size(), 3);
    QList<double> ordered;
    for (qsizetype idx : query.resultRows())
        ordered << query.rows().at(idx).properties.value(QStringLiteral("price")).number;
    EXPECT_EQ(ordered, QList<double>({10, 20, 30}));

    // Break the guard: descending must give the reverse, not the same order.
    query.sortBy(QStringLiteral("price"), SortDirection::Descending);
    QList<double> descending;
    for (qsizetype idx : query.resultRows())
        descending << query.rows().at(idx).properties.value(QStringLiteral("price")).number;
    EXPECT_EQ(descending, QList<double>({30, 20, 10}));
}

TEST(BaseQueryTest, GroupBySortsAheadOfTheViewsOwnSort)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name]\n"
        "    groupBy:\n      property: category\n      direction: ASC\n"
        "    sort:\n      - property: price\n        direction: ASC\n"));
    QList<Row> rows;
    struct Seed { QString cat; double price; };
    for (const Seed &s : {Seed{"b", 5}, Seed{"a", 9}, Seed{"b", 1}, Seed{"a", 2}}) {
        Row r = makeRow(QStringLiteral("N%1").arg(rows.size()));
        r.properties.insert(QStringLiteral("category"), Value::ofString(s.cat));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(s.price));
        rows << r;
    }
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    QStringList categories;
    QList<double> prices;
    for (qsizetype idx : query.resultRows()) {
        categories << query.rows().at(idx).properties.value(QStringLiteral("category")).text;
        prices << query.rows().at(idx).properties.value(QStringLiteral("price")).number;
    }
    // Both "a"s before both "b"s (grouped), each group sorted by price.
    EXPECT_EQ(categories, QStringList({"a", "a", "b", "b"}));
    EXPECT_EQ(prices, QList<double>({2, 9, 1, 5}));
}

TEST(BaseQueryTest, LimitKeepsTheFirstNAfterSort)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name]\n    limit: 2\n"
        "    sort:\n      - property: price\n        direction: DESC\n"));
    QList<Row> rows;
    for (int v : {10, 30, 20}) {
        Row r = makeRow(QStringLiteral("N%1").arg(v));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber(v));
        rows << r;
    }
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    ASSERT_EQ(query.resultRows().size(), 2);
    EXPECT_EQ(query.rows().at(query.resultRows().at(0)).properties.value(QStringLiteral("price")).number, 30);
    EXPECT_EQ(query.rows().at(query.resultRows().at(1)).properties.value(QStringLiteral("price")).number, 20);
}

TEST(BaseQueryTest, FormulasResolveAndMemoiseAndDetectCycles)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "formulas:\n"
        "  ppu: \"price / qty\"\n"
        "  bad: \"formula.bad + 1\"\n"
        "views:\n  - type: table\n    name: T\n    order: [formula.ppu]\n"));
    Row r = makeRow(QStringLiteral("N"));
    r.properties.insert(QStringLiteral("price"), Value::ofNumber(10));
    r.properties.insert(QStringLiteral("qty"), Value::ofNumber(2));
    QList<Row> rows{r};
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    EXPECT_DOUBLE_EQ(query.valueOf(0, QStringLiteral("formula.ppu")).number, 5);
    EXPECT_TRUE(query.valueOf(0, QStringLiteral("formula.bad")).isEmpty());
    EXPECT_FALSE(query.warnings().isEmpty()); // the cycle was reported, not silently swallowed
}

TEST(BaseQueryTest, ValueOfReadsFileNoteAndFormulaColumns)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "formulas:\n  double: \"price * 2\"\n"
        "views:\n  - type: table\n    name: T\n    order: [file.name, price, formula.double]\n"));
    Row r = makeRow(QStringLiteral("Home"));
    r.properties.insert(QStringLiteral("price"), Value::ofNumber(3));
    QList<Row> rows{r};
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));
    EXPECT_EQ(query.valueOf(0, QStringLiteral("file.name")).text, QStringLiteral("Home"));
    EXPECT_DOUBLE_EQ(query.valueOf(0, QStringLiteral("price")).number, 3);
    EXPECT_DOUBLE_EQ(query.valueOf(0, QStringLiteral("formula.double")).number, 6);
}

TEST(BaseQueryTest, LinksToResolvesThroughTheQuerysOwnRowsNotJustTheCurrentOne)
{
    // Hub.related -> [[A]] (bare, no folder); A.md itself links to B.md.
    // link.linksTo(file) asks whether *the file the link points to* (A) has
    // a link to `file` (B) - not whether the row being evaluated (Hub) does.
    Row hub = makeRow(QStringLiteral("Hub"));
    hub.properties.insert(QStringLiteral("related"), Value::ofValues({Value::ofLink(QStringLiteral("A"))}));
    Row a = makeRow(QStringLiteral("A"), QStringLiteral("Notes"));
    a.links = {QStringLiteral("B")};
    Row b = makeRow(QStringLiteral("B"), QStringLiteral("Notes"));

    const BaseFile file = parseOrDie(QStringLiteral("views:\n  - type: table\n    order: [file.name]\n"));
    QList<Row> rows{hub, a, b};
    BaseQuery query(file, rows);
    ASSERT_TRUE(query.openView(0));

    // rows() keeps the original order (hub=0, a=1, b=2) regardless of the
    // view's filter/sort - resultRows() is what's reordered.
    EXPECT_EQ(query.valueOf(0, QStringLiteral("related.map(value.linksTo(\"Notes/B\")).join(\",\")")).text,
             QStringLiteral("true"));
    EXPECT_EQ(query.valueOf(0, QStringLiteral("related.map(value.linksTo(\"Nope\")).join(\",\")")).text,
             QStringLiteral("false"));
    // Row A's own file.asLink().linksTo(...) needs no indirection at all.
    EXPECT_TRUE(query.valueOf(1, QStringLiteral("file.asLink().linksTo(\"B\")")).boolean);
}

// =========================================================================
// Index::allNotesForBases against a temp vault
// =========================================================================

namespace {

class BasesIndexTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        write(QStringLiteral("Home.md"),
              "---\nstatus: open\nprice: 12\ntags: [x, y]\n---\n\nSee [[Other]]. #inline\n");
        write(QStringLiteral("Other.md"), "# Other\n\nNothing here.\n");
        vault = Vault(dir.path());
    }
    void write(const QString &relative, const QByteArray &bytes)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    void build(Index &index)
    {
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }
    QTemporaryDir dir;
    Vault vault;
};

} // namespace

TEST_F(BasesIndexTest, ReturnsEveryNoteWithPropertiesTagsAndLinks)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QList<Index::BaseNote> notes = index.allNotesForBases();
    EXPECT_EQ(notes.size(), 2);

    const Index::BaseNote *home = nullptr;
    for (const Index::BaseNote &n : notes)
        if (n.path == QStringLiteral("Home.md")) home = &n;
    ASSERT_NE(home, nullptr);
    EXPECT_GT(home->mtime, 0);
    EXPECT_GT(home->size, 0);
    EXPECT_TRUE(home->tags.contains(QStringLiteral("inline")));

    bool sawStatus = false, sawPrice = false;
    for (const Property &p : home->properties) {
        if (p.key == QLatin1String("status")) { sawStatus = true; EXPECT_EQ(p.values.first(), QStringLiteral("open")); }
        if (p.key == QLatin1String("price")) { sawPrice = true; EXPECT_EQ(p.type, Property::Type::Number); }
    }
    EXPECT_TRUE(sawStatus);
    EXPECT_TRUE(sawPrice);

    const Row row = rowFromNote(*home);
    EXPECT_EQ(row.noteName, QStringLiteral("Home"));
    EXPECT_DOUBLE_EQ(row.properties.value(QStringLiteral("price")).number, 12);
}

// =========================================================================
// BaseTableModel: editing writes only that property's bytes
// =========================================================================

TEST_F(BasesIndexTest, EditingACellRewritesOnlyThatPropertysBytes)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QString before = [&] {
        QFile f(dir.filePath(QStringLiteral("Home.md")));
        [[maybe_unused]] const bool opened = f.open(QIODevice::ReadOnly);
        return QString::fromUtf8(f.readAll());
    }();

    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name, status, price]\n"));
    QList<Row> rows;
    for (const Index::BaseNote &n : index.allNotesForBases())
        rows << rowFromNote(n);
    BaseQuery query(file, rows, &vault);
    ASSERT_TRUE(query.openView(0));

    BaseTableModel model(&query, &index, &vault);
    int homeRow = -1;
    for (int i = 0; i < model.rowCount(); ++i)
        if (model.notePathAt(i) == QStringLiteral("Home.md")) homeRow = i;
    ASSERT_GE(homeRow, 0);

    const QModelIndex statusCell = model.index(homeRow, 1); // status column
    ASSERT_TRUE(model.flags(statusCell) & Qt::ItemIsEditable);
    ASSERT_TRUE(model.setData(statusCell, QStringLiteral("done")));

    const QString after = [&] {
        QFile f(dir.filePath(QStringLiteral("Home.md")));
        [[maybe_unused]] const bool opened = f.open(QIODevice::ReadOnly);
        return QString::fromUtf8(f.readAll());
    }();

    EXPECT_NE(before, after);
    EXPECT_TRUE(after.contains(QStringLiteral("status: done")));
    // Nothing else moved: nought but the status line differs.
    const QStringList beforeLines = before.split(QLatin1Char('\n'));
    const QStringList afterLines = after.split(QLatin1Char('\n'));
    ASSERT_EQ(beforeLines.size(), afterLines.size());
    int differing = 0;
    for (int i = 0; i < beforeLines.size(); ++i)
        if (beforeLines.at(i) != afterLines.at(i)) ++differing;
    EXPECT_EQ(differing, 1);

    // The index was refreshed too, not just the file.
    const QList<Index::BaseNote> refreshed = index.allNotesForBases();
    bool sawDone = false;
    for (const Index::BaseNote &n : refreshed)
        if (n.path == QStringLiteral("Home.md"))
            for (const Property &p : n.properties)
                if (p.key == QLatin1String("status")) sawDone = p.values.first() == QStringLiteral("done");
    EXPECT_TRUE(sawDone);
}

TEST_F(BasesIndexTest, ComputedColumnsAreNotEditableButListsNowAre)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const BaseFile file = parseOrDie(QStringLiteral(
        "formulas:\n  double: \"price * 2\"\n"
        "views:\n  - type: table\n    name: T\n    order: [file.name, tags, formula.double]\n"));
    QList<Row> rows;
    for (const Index::BaseNote &n : index.allNotesForBases())
        rows << rowFromNote(n);
    BaseQuery query(file, rows, &vault);
    ASSERT_TRUE(query.openView(0));
    BaseTableModel model(&query, &index, &vault);

    int homeRow = -1;
    for (int i = 0; i < model.rowCount(); ++i)
        if (model.notePathAt(i) == QStringLiteral("Home.md")) homeRow = i;
    ASSERT_GE(homeRow, 0);

    EXPECT_FALSE(model.flags(model.index(homeRow, 0)) & Qt::ItemIsEditable); // file.name
    EXPECT_TRUE(model.flags(model.index(homeRow, 1)) & Qt::ItemIsEditable);  // tags: List - now editable
    EXPECT_FALSE(model.flags(model.index(homeRow, 2)) & Qt::ItemIsEditable); // formula.double
    EXPECT_TRUE(model.isNameColumn(0));
}

// =========================================================================
// List-property cells: chips, editable, byte-exact write-back
// =========================================================================

TEST_F(BasesIndexTest, ListCellShowsChipsAndEditingWritesBackAsAYAMLList)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QString before = [&] {
        QFile f(dir.filePath(QStringLiteral("Home.md")));
        [[maybe_unused]] const bool opened = f.open(QIODevice::ReadOnly);
        return QString::fromUtf8(f.readAll());
    }();
    ASSERT_TRUE(before.contains(QStringLiteral("tags: [x, y]")));

    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name, tags]\n"));
    QList<Row> rows;
    for (const Index::BaseNote &n : index.allNotesForBases())
        rows << rowFromNote(n);
    BaseQuery query(file, rows, &vault);
    ASSERT_TRUE(query.openView(0));
    BaseTableModel model(&query, &index, &vault);

    int homeRow = -1;
    for (int i = 0; i < model.rowCount(); ++i)
        if (model.notePathAt(i) == QStringLiteral("Home.md")) homeRow = i;
    ASSERT_GE(homeRow, 0);
    const QModelIndex tagsCell = model.index(homeRow, 1);

    // Chips: the raw Value is still a List (ChipRole's job, not text).
    const QVariant chips = model.data(tagsCell, BaseTableModel::ChipRole);
    ASSERT_TRUE(chips.canConvert<QList<BaseChip>>());
    const QList<BaseChip> chipList = chips.value<QList<BaseChip>>();
    ASSERT_EQ(chipList.size(), 2);
    EXPECT_EQ(chipList.at(0).text, QStringLiteral("x"));
    EXPECT_FALSE(chipList.at(0).isLink);

    // Editing: the default editor's comma-joined EditRole text...
    EXPECT_EQ(model.data(tagsCell, Qt::EditRole).toString(), QStringLiteral("x, y"));
    // ...splits back into a list on commit, rewritten byte-exact.
    ASSERT_TRUE(model.flags(tagsCell) & Qt::ItemIsEditable);
    ASSERT_TRUE(model.setData(tagsCell, QStringLiteral("a, b, c")));

    const QString after = [&] {
        QFile f(dir.filePath(QStringLiteral("Home.md")));
        [[maybe_unused]] const bool opened = f.open(QIODevice::ReadOnly);
        return QString::fromUtf8(f.readAll());
    }();
    EXPECT_TRUE(after.contains(QStringLiteral("tags: [a, b, c]"))) << after.toStdString();
    const QStringList beforeLines = before.split(QLatin1Char('\n'));
    const QStringList afterLines = after.split(QLatin1Char('\n'));
    ASSERT_EQ(beforeLines.size(), afterLines.size());
    int differing = 0;
    for (int i = 0; i < beforeLines.size(); ++i)
        if (beforeLines.at(i) != afterLines.at(i)) ++differing;
    EXPECT_EQ(differing, 1) << "only the tags: line should have moved";
}

// =========================================================================
// BaseView smoke test (offscreen)
// =========================================================================

TEST_F(BasesIndexTest, BaseViewOpensAndSwitchesViews)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n    name: Table\n    order: [file.name]\n"
        "  - type: cards\n    name: Cards\n    order: [file.name]\n");
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")), yaml)) << view.lastError().toStdString();
    ASSERT_TRUE(opened.wait(5000));
    EXPECT_TRUE(view.warnings().isEmpty());
    auto *table = view.findChild<QTableView *>();
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->model()->rowCount(), 0);

    // The second view, and its name as the switcher shows it.
    auto *switcher = view.findChild<QComboBox *>();
    ASSERT_EQ(switcher->count(), 2);
    EXPECT_EQ(switcher->itemText(1), QStringLiteral("Cards"));
    switcher->setCurrentIndex(1);
    ASSERT_TRUE(opened.wait(5000));
}

TEST_F(BasesIndexTest, TheTableKeepsTheBasesOwnSortOrder)
{
    write(QStringLiteral("Task1.md"), "---\npriority: 2\n---\n");
    write(QStringLiteral("Task2.md"), "---\npriority: 3\n---\n");
    write(QStringLiteral("Task3.md"), "---\npriority: 1\n---\n");
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    view.show(); // the header sorts only a shown table
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("filters:\n  and:\n    - priority > 0\nviews:\n  - type: table\n"
                                         "    order: [file.name, priority]\n"
                                         "    sort:\n      - property: priority\n        direction: ASC\n")));
    ASSERT_TRUE(opened.wait(5000));
    auto *table = view.findChild<QTableView *>();
    ASSERT_EQ(table->model()->rowCount(), 3);
    // Not re-sorted by the header's default (the name, descending).
    EXPECT_EQ(table->model()->index(0, 0).data().toString(), QStringLiteral("Task3"));
    EXPECT_EQ(table->model()->index(1, 0).data().toString(), QStringLiteral("Task1"));
    EXPECT_EQ(table->model()->index(2, 0).data().toString(), QStringLiteral("Task2"));
}

TEST_F(BasesIndexTest, AViewWithoutASortKeepsTheNotesOwnOrder)
{
    write(QStringLiteral("Task1.md"), "---\npriority: 2\n---\n");
    write(QStringLiteral("Task2.md"), "---\npriority: 3\n---\n");
    write(QStringLiteral("Task3.md"), "---\npriority: 1\n---\n");
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    view.show();
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("filters:\n  and:\n    - priority > 0\nviews:\n  - type: table\n"
                                         "    order: [file.name, priority]\n")));
    ASSERT_TRUE(opened.wait(5000));
    auto *table = view.findChild<QTableView *>();
    ASSERT_EQ(table->model()->rowCount(), 3);
    EXPECT_EQ(table->model()->index(0, 0).data().toString(), QStringLiteral("Task1")); // was Task3
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), -1);

    // A header click still sorts.
    table->horizontalHeader()->setSortIndicator(1, Qt::AscendingOrder);
    EXPECT_EQ(table->model()->index(0, 0).data().toString(), QStringLiteral("Task3"));
}

TEST_F(BasesIndexTest, AnUnnamedViewIsNumberedByItsPlace)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("views:\n  - type: table\n  - type: table\n")));
    auto *switcher = view.findChild<QComboBox *>();
    ASSERT_EQ(switcher->count(), 2);
    EXPECT_EQ(switcher->itemText(1), QStringLiteral("View 2")); // was "View 3"
}

// =========================================================================
// Link cells: a click opens the note (BaseView::noteActivated)
// =========================================================================

TEST_F(BasesIndexTest, ClickingALinkCellOpensTheNoteItPointsTo)
{
    write(QStringLiteral("LinkNote.md"), "---\nrelated: \"[[Other]]\"\n---\n");
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("views:\n  - type: table\n    order: [file.name, related]\n")));
    ASSERT_TRUE(opened.wait(5000));

    auto *table = view.findChild<QTableView *>();
    int linkRow = -1;
    for (int i = 0; i < table->model()->rowCount(); ++i)
        if (table->model()->index(i, 0).data().toString() == QStringLiteral("LinkNote")) linkRow = i;
    ASSERT_GE(linkRow, 0);

    QSignalSpy activated(&view, &BaseView::noteActivated);
    table->clicked(table->model()->index(linkRow, 1)); // the related cell, not file.name
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.constFirst().constFirst().toString(), QStringLiteral("Other.md"));
}

// =========================================================================
// Sort write-back: a header click updates the .base file's sort: on disk
// =========================================================================

TEST_F(BasesIndexTest, HeaderClickPersistsSortIntoTheBaseFileByteExact)
{
    write(QStringLiteral("Task1.md"), "---\npriority: 2\n---\n");
    write(QStringLiteral("Task2.md"), "---\npriority: 3\n---\n");
    const QString baseYaml = QStringLiteral(
        "# a hand-written comment\n"
        "views:\n"
        "  - type: table\n"
        "    name: T\n"
        "    order: [file.name, priority]\n");
    write(QStringLiteral("x.base"), baseYaml.toUtf8());

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    view.show(); // header sorting only wires up on a shown table
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")), baseYaml));
    ASSERT_TRUE(opened.wait(5000));

    auto *table = view.findChild<QTableView *>();
    // Synchronous: onHeaderSortChanged writes with QSaveFile before
    // sortIndicatorChanged's signal delivery returns, no polling needed.
    table->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder); // priority column

    QFile saved(dir.filePath(QStringLiteral("x.base")));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QString onDisk = QString::fromUtf8(saved.readAll());

    EXPECT_TRUE(onDisk.startsWith(QStringLiteral("# a hand-written comment\n"))) << onDisk.toStdString();
    EXPECT_TRUE(onDisk.contains(QStringLiteral("- property: priority"))) << onDisk.toStdString();
    EXPECT_TRUE(onDisk.contains(QStringLiteral("direction: DESC"))) << onDisk.toStdString();
    // Byte-exact elsewhere: only the new sort: block was added, nothing that
    // was already there moved - so the original text is still a substring
    // starting at byte 0 and running up to where the addition begins.
    const qsizetype orderEnd = baseYaml.indexOf(QStringLiteral("order: [file.name, priority]\n"))
                              + QStringLiteral("order: [file.name, priority]\n").size();
    EXPECT_EQ(onDisk.left(orderEnd), baseYaml.left(orderEnd));

    // Re-parsing the saved file shows the same sort the header now has.
    const BaseFile reparsed = parseOrDie(onDisk);
    ASSERT_FALSE(reparsed.views().first().sort.isEmpty());
    EXPECT_EQ(reparsed.views().first().sort.first().property, QStringLiteral("priority"));
    EXPECT_EQ(reparsed.views().first().sort.first().direction, SortDirection::Descending);
}

TEST_F(BasesIndexTest, ABaseBlockWithNoPathOfItsOwnIsNeverRewritten)
{
    // BaseView's own path is empty unless open() was given one - exactly
    // what happens for a note's ```base``` block, evaluated through
    // BaseEditorTable instead of a BaseView at all (see MainWindow.cpp).
    // This checks the *BaseView* side of that decision: passing an empty
    // path must not attempt to write anywhere.
    write(QStringLiteral("Task1.md"), "---\npriority: 2\n---\n");
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    view.show();
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(QString(),
                          QStringLiteral("views:\n  - type: table\n    order: [file.name, priority]\n")));
    ASSERT_TRUE(opened.wait(5000));
    auto *table = view.findChild<QTableView *>();
    table->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("x.base"))));
}

// =========================================================================
// Cards: an image loads off the UI thread and is cached; fields are
// editable through the same write path as the table
// =========================================================================

TEST_F(BasesIndexTest, CardsLoadAnImageOffThreadAndCacheIt)
{
    QImage cover(4, 4, QImage::Format_RGB32);
    cover.fill(Qt::red);
    ASSERT_TRUE(cover.save(dir.filePath(QStringLiteral("cover.png"))));
    write(QStringLiteral("Illustrated.md"), "---\nprice: 5\n---\n");

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    QSignalSpy opened(&view, &BaseView::viewOpened);
    // Every note's cover is the same plain relative path - a formula gives
    // every row that value without needing a real "cover" property.
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("formulas:\n  cover: \"'cover.png'\"\n"
                                         "views:\n  - type: cards\n    order: [file.name]\n    image: formula.cover\n")));
    ASSERT_TRUE(opened.wait(5000));

    auto labels = view.findChildren<QLabel *>();
    QLabel *coverLabel = nullptr;
    for (QLabel *l : labels)
        if (l->size() == QSize(150, 90)) coverLabel = l;
    ASSERT_NE(coverLabel, nullptr);
    WAIT_TRUE(!coverLabel->pixmap().isNull(), 5000);
    // KeepAspectRatioByExpanding covers the 150x90 target without distorting
    // a square source, which is why the result is 150x150, not 150x90 -
    // this is the label's own fixed size clipping the overflow, not a bug.
    EXPECT_GE(coverLabel->pixmap().width(), 150);
    EXPECT_GE(coverLabel->pixmap().height(), 90);
}

TEST_F(BasesIndexTest, CardFieldsEditThroughTheSameWritePathAsTheTable)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    BaseView view;
    view.setIndex(&index);
    view.setVault(&vault);
    QSignalSpy opened(&view, &BaseView::viewOpened);
    ASSERT_TRUE(view.open(dir.filePath(QStringLiteral("x.base")),
                          QStringLiteral("views:\n  - type: cards\n    order: [file.name, status]\n")));
    ASSERT_TRUE(opened.wait(5000));

    auto edits = view.findChildren<QLineEdit *>();
    QLineEdit *statusEdit = nullptr;
    for (QLineEdit *e : edits)
        if (e->text() == QStringLiteral("open")) statusEdit = e;
    ASSERT_NE(statusEdit, nullptr) << "Home.md's status field should be an editable line";

    QSignalSpy edited(&view, &BaseView::viewOpened); // onCellEdited() reruns the query, which re-emits this
    statusEdit->setText(QStringLiteral("done"));
    emit statusEdit->editingFinished();
    ASSERT_TRUE(edited.wait(5000));

    QFile f(dir.filePath(QStringLiteral("Home.md")));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_TRUE(QString::fromUtf8(f.readAll()).contains(QStringLiteral("status: done")));
}

// =========================================================================
// evaluateBaseForEditor: the one function libs/editor's ```base block calls
// (on a worker thread - see MarkdownEditor::setBaseRunner), so tested here
// with hand-built Index::BaseNote rows rather than a real Index: the point
// of the function is that it needs neither a database nor a widget.
// =========================================================================

namespace {

Index::BaseNote makeBaseNote(const QString &path, qint64 mtime = 1000, qint64 size = 100)
{
    Index::BaseNote note;
    note.path = path;
    note.mtime = mtime;
    note.size = size;
    return note;
}

void addProperty(Index::BaseNote &note, const QString &key, Property::Type type,
                 const QStringList &values)
{
    Property p;
    p.key = key;
    p.type = type;
    p.values = values;
    note.properties << p;
}

} // namespace

// =========================================================================
// rowFromNote: a property holding [[Note]] (or a list of them) becomes a
// Link value - obsidian.md/help/bases/syntax: "Links in frontmatter
// properties are automatically recognized as Link objects".
// =========================================================================

TEST(BaseQueryRowTest, WikilinkPropertiesBecomeLinkValues)
{
    Index::BaseNote note = makeBaseNote(QStringLiteral("A.md"));
    addProperty(note, QStringLiteral("author"), Property::Type::Text, {QStringLiteral("[[Textbook]]")});
    addProperty(note, QStringLiteral("aliased"), Property::Type::Text, {QStringLiteral("[[Textbook|My Book]]")});
    addProperty(note, QStringLiteral("mentions"), Property::Type::Text,
               {QStringLiteral("see [[Textbook]] for more")}); // not the *whole* value: stays text
    addProperty(note, QStringLiteral("related"), Property::Type::List,
               {QStringLiteral("[[A]]"), QStringLiteral("plain text"), QStringLiteral("[[B|Beta]]")});

    const Row row = rowFromNote(note);

    const Value author = row.properties.value(QStringLiteral("author"));
    EXPECT_EQ(author.type, Value::Type::Link);
    EXPECT_EQ(author.text, QStringLiteral("Textbook"));
    EXPECT_EQ(author.toDisplayString(), QStringLiteral("Textbook")); // no alias: shows the raw target

    const Value aliased = row.properties.value(QStringLiteral("aliased"));
    EXPECT_EQ(aliased.type, Value::Type::Link);
    EXPECT_EQ(aliased.text, QStringLiteral("Textbook"));
    EXPECT_EQ(aliased.toDisplayString(), QStringLiteral("My Book"));

    const Value mentions = row.properties.value(QStringLiteral("mentions"));
    EXPECT_EQ(mentions.type, Value::Type::String);
    EXPECT_EQ(mentions.text, QStringLiteral("see [[Textbook]] for more"));

    const Value related = row.properties.value(QStringLiteral("related"));
    ASSERT_EQ(related.type, Value::Type::List);
    ASSERT_EQ(related.list.size(), 3);
    EXPECT_EQ(related.list.at(0).type, Value::Type::Link);
    EXPECT_EQ(related.list.at(0).text, QStringLiteral("A"));
    EXPECT_EQ(related.list.at(1).type, Value::Type::String);
    EXPECT_EQ(related.list.at(1).text, QStringLiteral("plain text"));
    EXPECT_EQ(related.list.at(2).type, Value::Type::Link);
    EXPECT_EQ(related.list.at(2).toDisplayString(), QStringLiteral("Beta"));
}

TEST(BaseEditorTableTest, BuildsHeadersAndRowsFromTheFirstView)
{
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n    name: Prices\n    order: [file.name, price]\n");

    Index::BaseNote a = makeBaseNote(QStringLiteral("A.md"));
    addProperty(a, QStringLiteral("price"), Property::Type::Number, {QStringLiteral("12")});
    Index::BaseNote b = makeBaseNote(QStringLiteral("Folder/B.md"));
    addProperty(b, QStringLiteral("price"), Property::Type::Number, {QStringLiteral("7")});

    const bases::BaseEditorTable table = bases::evaluateBaseForEditor(yaml, {a, b});

    EXPECT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    ASSERT_EQ(table.viewNames.size(), 1);
    EXPECT_EQ(table.viewNames.first(), QStringLiteral("Prices"));
    EXPECT_EQ(table.shownView, 0);
    ASSERT_EQ(table.headers.size(), 2);
    EXPECT_EQ(table.headers.at(0), QStringLiteral("Name")); // displayNameFor("file.name")
    EXPECT_EQ(table.headers.at(1), QStringLiteral("Price"));

    ASSERT_EQ(table.rows.size(), 2);
    for (const bases::BaseEditorRow &row : table.rows) {
        ASSERT_EQ(row.cells.size(), 2);
        if (row.notePath == QStringLiteral("A.md")) {
            EXPECT_EQ(row.cells.at(0), QStringLiteral("A"));
            EXPECT_EQ(row.cells.at(1), QStringLiteral("12")); // Value::toDisplayString: no ".0"
        } else {
            EXPECT_EQ(row.notePath, QStringLiteral("Folder/B.md"));
            EXPECT_EQ(row.cells.at(1), QStringLiteral("7"));
        }
    }
}

TEST(BaseEditorTableTest, FormatsEveryColumnTypeAsPlainTextIncludingBooleans)
{
    // BaseTableModel::data() shows a Bool column as a checkbox (empty
    // DisplayRole text - see BaseTableModel.cpp), but there is no checkbox
    // in a plain editor table, so this function's job is to format it as
    // text too - Value::toDisplayString() already knows how ("true"/
    // "false"), the same call BaseTableModel makes for every other type.
    const QString yaml = QStringLiteral(
        "formulas:\n  expensive: \"price > 10\"\n"
        "views:\n  - type: table\n    name: T\n    order: [file.name, formula.expensive]\n");
    Index::BaseNote note = makeBaseNote(QStringLiteral("A.md"));
    addProperty(note, QStringLiteral("price"), Property::Type::Number, {QStringLiteral("12")});

    const bases::BaseEditorTable table = bases::evaluateBaseForEditor(yaml, {note});
    ASSERT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    ASSERT_EQ(table.rows.size(), 1);
    EXPECT_EQ(table.rows.first().cells.at(1), QStringLiteral("true"));
}

TEST(BaseEditorTableTest, InvalidYAMLSurfacesAsError)
{
    const bases::BaseEditorTable table =
        bases::evaluateBaseForEditor(QStringLiteral("- just\n- a\n- list\n"), {});
    EXPECT_FALSE(table.error.isEmpty());
    EXPECT_TRUE(table.rows.isEmpty());
    EXPECT_TRUE(table.headers.isEmpty());
}

TEST(BaseEditorTableTest, InvalidFilterSyntaxSurfacesAsErrorNotEmptyResults)
{
    const QString yaml = QStringLiteral(
        "filters:\n  and:\n    - 'price >'\nviews:\n  - type: table\n    name: T\n");
    const bases::BaseEditorTable table =
        bases::evaluateBaseForEditor(yaml, {makeBaseNote(QStringLiteral("A.md"))});
    EXPECT_FALSE(table.error.isEmpty());
}

TEST(BaseEditorTableTest, OutOfRangeViewIndexFallsBackToTheFirstView)
{
    const QString yaml = QStringLiteral(
        "views:\n  - type: table\n    name: Only\n    order: [file.name]\n");
    const bases::BaseEditorTable table =
        bases::evaluateBaseForEditor(yaml, {makeBaseNote(QStringLiteral("A.md"))}, nullptr, 99);
    EXPECT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    EXPECT_EQ(table.shownView, 0);
    EXPECT_EQ(table.rows.size(), 1);
}

TEST(BaseEditorTableTest, ViewNameSelectsThatViewCaseInsensitively)
{
    // ![[Books.base#Reading]] - the fragment after "#" names a view; the
    // editor passes it straight through as viewName rather than resolving
    // it to an index itself, so this is where the name lookup happens.
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n    name: To read\n    order: [file.name]\n"
        "  - type: table\n    name: Reading\n    order: [file.name, price]\n");
    Index::BaseNote note = makeBaseNote(QStringLiteral("A.md"));
    addProperty(note, QStringLiteral("price"), Property::Type::Number, {QStringLiteral("12")});

    const bases::BaseEditorTable table =
        bases::evaluateBaseForEditor(yaml, {note}, nullptr, -1, QStringLiteral("reading"));
    EXPECT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    EXPECT_EQ(table.shownView, 1);
    ASSERT_EQ(table.headers.size(), 2);
    EXPECT_EQ(table.headers.at(1), QStringLiteral("Price"));
}

TEST(BaseEditorTableTest, UnknownViewNameFallsBackToTheFirstView)
{
    const QString yaml = QStringLiteral(
        "views:\n"
        "  - type: table\n    name: To read\n    order: [file.name]\n"
        "  - type: table\n    name: Reading\n    order: [file.name, price]\n");
    const bases::BaseEditorTable table = bases::evaluateBaseForEditor(
        yaml, {makeBaseNote(QStringLiteral("A.md"))}, nullptr, -1, QStringLiteral("Archived"));
    EXPECT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    EXPECT_EQ(table.shownView, 0);
}

TEST(BaseEditorTableTest, NoViewsGetsTheImplicitAllNotesTableWithNoColumns)
{
    // BaseFile::parse gives a base with no `views:` one implicit unfiltered
    // table (BaseFileTest.NoViewsGetsAnImplicitTable), but that implicit
    // view has no `order:` either - the same degenerate case
    // BaseTableModel::columnCount() already shows as zero columns, mirrored
    // here rather than solved differently.
    const bases::BaseEditorTable table =
        bases::evaluateBaseForEditor(QString(), {makeBaseNote(QStringLiteral("A.md"))});
    EXPECT_TRUE(table.error.isEmpty()) << table.error.toStdString();
    ASSERT_EQ(table.viewNames.size(), 1);
    EXPECT_EQ(table.viewNames.first(), QStringLiteral("All notes"));
    EXPECT_TRUE(table.headers.isEmpty());
}

// =========================================================================
// Performance guard
// =========================================================================

namespace {

QList<Row> generateRows(int count)
{
    QList<Row> rows;
    rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        Row r = makeRow(QStringLiteral("Note%1").arg(i), i % 20 == 0 ? QString() : QStringLiteral("Folder%1").arg(i % 15));
        r.properties.insert(QStringLiteral("price"), Value::ofNumber((i * 37) % 500));
        r.properties.insert(QStringLiteral("qty"), Value::ofNumber(1 + (i % 9)));
        r.properties.insert(QStringLiteral("status"), Value::ofString(i % 3 == 0 ? QStringLiteral("done") : QStringLiteral("open")));
        r.tags = {QStringLiteral("tag%1").arg(i % 12)};
        rows << r;
    }
    return rows;
}

qint64 timeOpen(const BaseFile &file, const QList<Row> &rows)
{
    BaseQuery query(file, rows);
    QElapsedTimer timer;
    timer.start();
    QString error;
    const bool ok = query.openView(0, &error);
    const qint64 elapsed = timer.elapsed();
    EXPECT_TRUE(ok) << error.toStdString();
    return elapsed;
}

} // namespace

TEST(BasesPerformanceTest, TwentyThousandRowsFilterAndSortWellUnderBudget)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "formulas:\n  ppu: \"price / qty\"\n"
        "views:\n  - type: table\n    name: T\n    order: [file.name, formula.ppu]\n"
        "    filters:\n      and:\n        - 'status != \"done\"'\n        - 'price > 10'\n"
        "    sort:\n      - property: formula.ppu\n        direction: DESC\n"));
    const QList<Row> rows = generateRows(20000);
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three: load only ever adds time.
    const qint64 elapsed = perf::bestOf([&] { return timeOpen(file, rows); }, 3, 100);
    // "Well under 100 ms" per the task; 100 ms is the hard ceiling checked
    // here so a real regression fails the build without flaking on a loaded
    // CI box.
    EXPECT_LT(elapsed, 100) << "20000-row filter+sort took " << elapsed << " ms";
}

TEST(BasesPerformanceTest, TimeGrowsRoughlyLinearlyNotQuadratically)
{
    const BaseFile file = parseOrDie(QStringLiteral(
        "views:\n  - type: table\n    name: T\n    order: [file.name]\n"
        "    sort:\n      - property: price\n        direction: ASC\n"));
    const QList<Row> small = generateRows(2000);
    const QList<Row> big = generateRows(20000); // 10x the rows

    // Keep the fastest of three: a scheduler hiccup on one run should not
    // make an O(n log n) engine look quadratic.
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    qint64 smallTime = perf::bestOf([&] { return timeOpen(file, small); });
    qint64 bigTime = perf::bestOf([&] { return timeOpen(file, big); });
    smallTime = std::max<qint64>(smallTime, 1); // avoid a divide-by-zero on a very fast box

    // O(n log n) growing 10x in n grows a little over 10x; a quadratic
    // implementation would grow close to 100x. 40x is a generous guard that
    // still catches the quadratic case broken deliberately while writing
    // this test (removing the per-row value cache in sortBy() made this
    // fail at well over 100x).
    EXPECT_LT(bigTime, smallTime * 40)
        << "small=" << smallTime << "ms big=" << bigTime << "ms - looks worse than n log n";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
