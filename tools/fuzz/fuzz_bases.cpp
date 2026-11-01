// libFuzzer target: a .base file (YAML) and the expressions it holds,
// parsed and evaluated against one row of every value type.

#include <QDateTime>
#include <QString>

#include "BaseFile.h"
#include "Expression.h"

using namespace noteAppC::bases;

namespace {

class NoFormulas : public FormulaSource {
public:
    Value resolveFormula(const QString &, const Row &, QString *error) const override
    {
        if (error)
            *error = QStringLiteral("no formulas");
        return {};
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0)
        return 0;
    // The first byte picks the file or an expression; the rest is the text.
    const bool asFile = data[0] & 1;
    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(data) + 1, static_cast<qsizetype>(size) - 1);

    if (asFile) {
        if (const auto file = BaseFile::parse(text))
            file->serialize();
        BaseFile::rewriteViewSort(text, 0, QStringLiteral("note.x"), SortDirection::Ascending);
        return 0;
    }

    const auto expression = Expression::parse(text);
    if (!expression)
        return 0;
    Row row;
    row.noteName = QStringLiteral("Note");
    row.notePath = QStringLiteral("a/Note.md");
    row.noteFolder = QStringLiteral("a");
    row.noteExt = QStringLiteral("md");
    row.fileSize = 1234;
    row.fileCtime = QDateTime(QDate(2026, 1, 2), QTime(3, 4));
    row.fileMtime = QDateTime(QDate(2026, 5, 6), QTime(7, 8));
    row.tags = {QStringLiteral("#a"), QStringLiteral("#b/c")};
    row.links = {QStringLiteral("Other"), QStringLiteral("a/Note")};
    row.properties.insert(QStringLiteral("s"), Value::ofString(QStringLiteral("text")));
    row.properties.insert(QStringLiteral("n"), Value::ofNumber(42));
    row.properties.insert(QStringLiteral("b"), Value::ofBool(true));
    row.properties.insert(QStringLiteral("d"), Value::ofDate(row.fileMtime));
    row.properties.insert(QStringLiteral("l"), Value::ofList({QStringLiteral("x"), QStringLiteral("y")}));
    row.properties.insert(QStringLiteral("k"), Value::ofLink(QStringLiteral("Other")));
    const NoFormulas formulas;
    QString error;
    expression->evaluate(row, &formulas, QDateTime(QDate(2026, 9, 23), QTime(12, 0)), &error);
    return 0;
}
