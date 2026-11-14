// SPDX-License-Identifier: GPL-3.0-or-later
#include "CounterWord.h"

#include <QRegularExpression>

#include <algorithm>

namespace noteAppC {

/// A counter in the first word of a bullet item, `RA.1`, `1RA`, `v09`, or a
/// letter on its own, `a)`. Returns the word one step on, or an empty string
/// when the word is no counter. Everything but the last digit run (or the
/// letter) is kept as it is, and zero padding keeps its width.
QString nextCounterWord(const QString &word)
{
    static const QRegularExpression letter(QStringLiteral("^([A-Za-z])([.)])$"));
    const auto match = letter.match(word);
    if (match.hasMatch()) {
        const QChar c = match.captured(1).at(0);
        return c == QLatin1Char('z') || c == QLatin1Char('Z') ? QString()
                                                               : QString(QChar(c.unicode() + 1)) + match.captured(2);
    }
    // A link, a URL or code is no counter, however many digits it holds.
    if (word.contains(QLatin1String("[[")) || word.contains(QLatin1String("](")) || word.contains(QLatin1String("://"))
        || word.startsWith(QLatin1Char('`')) || word.startsWith(QLatin1Char('<')) || word.startsWith(QLatin1Char('!')))
        return {};
    const auto isDigit = [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); };
    qsizetype end = word.size();
    while (end > 0 && !isDigit(word.at(end - 1)))
        --end;
    if (end == 0)
        return {};
    qsizetype start = end;
    while (start > 0 && isDigit(word.at(start - 1)))
        --start;
    QString run = word.mid(start, end - start);
    // Add one by hand: any length, and the padding stays.
    qsizetype at = run.size() - 1;
    while (at >= 0 && run.at(at) == QLatin1Char('9'))
        run[at--] = QLatin1Char('0');
    if (at >= 0)
        run[at] = QChar(run.at(at).unicode() + 1);
    else
        run.prepend(QLatin1Char('1'));
    return word.left(start) + run + word.mid(end);
}

namespace TableCounters {

namespace {

/// "ID", "Nr.", "(No)", "Student number": any whole word of the header.
bool keywordHeader(const QString &header)
{
    static const QStringList words = {
        QStringLiteral("id"),    QStringLiteral("no"),     QStringLiteral("no."),    QStringLiteral("nr"),
        QStringLiteral("nr."),   QStringLiteral("num"),    QStringLiteral("number"), QStringLiteral("#"),
        QStringLiteral("count"), QStringLiteral("index"),  QStringLiteral("idx"),    QStringLiteral("step"),
        QStringLiteral("rank"),  QStringLiteral("pos"),    QStringLiteral("position"), QStringLiteral("st."),
        QStringLiteral("\u0161t"), QStringLiteral("\u0161t.")};
    static const QString edges = QStringLiteral("()[]{}:;,");
    const QStringList tokens = header.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString token : tokens) {
        while (!token.isEmpty() && edges.contains(token.front()))
            token.remove(0, 1);
        while (!token.isEmpty() && edges.contains(token.back()))
            token.chop(1);
        token = token.toLower();
        if (words.contains(token) || (token.endsWith(QLatin1Char('.')) && words.contains(token.chopped(1))))
            return true;
    }
    return false;
}

bool hasDigit(const QString &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); });
}

/// Writes `seed` into `line`, then each later row one on, as far as it goes.
void numberFrom(QList<QStringList> &rows, int column, int line, QString value)
{
    for (int r = line; r < rows.size() && !value.isEmpty(); ++r) {
        if (column < rows.at(r).size())
            rows[r][column] = value;
        value = nextCounterWord(value);
    }
}

} // namespace

QList<int> columns(const QList<QStringList> &rows)
{
    QList<int> found;
    if (rows.size() < 2)
        return found;
    for (int c = 0; c < rows.first().size(); ++c) {
        const bool keyword = keywordHeader(rows.first().at(c));
        if (c != 0 && !keyword)
            continue;
        QStringList body;
        for (int r = 2; r < rows.size(); ++r)
            body << rows.at(r).value(c).trimmed();
        const bool allEmpty = std::all_of(body.cbegin(), body.cend(), [](const QString &cell) { return cell.isEmpty(); });
        if (allEmpty) {
            if (keyword)
                found << c;
            continue;
        }
        bool sequence = (keyword || hasDigit(body.first())) && !nextCounterWord(body.first()).isEmpty();
        for (int i = 1; sequence && i < body.size(); ++i)
            sequence = nextCounterWord(body.at(i - 1)) == body.at(i);
        if (sequence)
            found << c;
    }
    return found;
}

void afterInsert(QList<QStringList> &rows, int line, const QList<int> &counters)
{
    for (const int c : counters) {
        const bool allEmpty = std::all_of(rows.cbegin() + 2, rows.cend(), [c](const QStringList &row) {
            return row.value(c).trimmed().isEmpty();
        });
        if (allEmpty) { // a keyword column with nothing in it yet starts at 1
            numberFrom(rows, c, 2, QStringLiteral("1"));
            continue;
        }
        // The slot's old value goes to the new row, and the rest follow it.
        QString seed = line + 1 < rows.size() ? rows.at(line + 1).value(c).trimmed() : QString();
        if (seed.isEmpty() && line > 2)
            seed = nextCounterWord(rows.at(line - 1).value(c).trimmed());
        numberFrom(rows, c, line, seed);
    }
}

void afterDelete(QList<QStringList> &rows, int line, const QStringList &removed, const QList<int> &counters)
{
    for (const int c : counters)
        numberFrom(rows, c, line, removed.value(c).trimmed());
}

} // namespace TableCounters

} // namespace noteAppC
