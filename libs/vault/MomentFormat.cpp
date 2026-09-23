#include "MomentFormat.h"

#include <QDate>
#include <QLocale>
#include <QTime>

namespace noteAppC::MomentFormat {

namespace {

QString ordinalSuffix(int day)
{
    if (day % 100 >= 11 && day % 100 <= 13)
        return QStringLiteral("th");
    switch (day % 10) {
    case 1:
        return QStringLiteral("st");
    case 2:
        return QStringLiteral("nd");
    case 3:
        return QStringLiteral("rd");
    default:
        return QStringLiteral("th");
    }
}

/// English names regardless of the user's locale, the same as moment.js's own
/// default - Obsidian does not localise these either.
QString expand(const QString &token, const QDate &date, const QTime &time)
{
    static const QLocale c(QLocale::English, QLocale::UnitedStates);

    if (token == QLatin1String("YYYY"))
        return QString::number(date.year()).rightJustified(4, QLatin1Char('0'));
    if (token == QLatin1String("YY"))
        return QString::number(date.year() % 100).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("MMMM"))
        return c.monthName(date.month(), QLocale::LongFormat);
    if (token == QLatin1String("MMM"))
        return c.monthName(date.month(), QLocale::ShortFormat);
    if (token == QLatin1String("MM"))
        return QString::number(date.month()).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("M"))
        return QString::number(date.month());
    if (token == QLatin1String("DD"))
        return QString::number(date.day()).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("Do"))
        return QString::number(date.day()) + ordinalSuffix(date.day());
    if (token == QLatin1String("D"))
        return QString::number(date.day());
    if (token == QLatin1String("dddd"))
        return c.dayName(date.dayOfWeek(), QLocale::LongFormat);
    if (token == QLatin1String("ddd"))
        return c.dayName(date.dayOfWeek(), QLocale::ShortFormat);
    if (token == QLatin1String("E"))
        return QString::number(date.dayOfWeek());
    if (token == QLatin1String("W") || token == QLatin1String("WW")) {
        int isoYear = 0;
        const int week = date.weekNumber(&isoYear);
        return token == QLatin1String("WW") ? QString::number(week).rightJustified(2, QLatin1Char('0'))
                                            : QString::number(week);
    }
    if (token == QLatin1String("gggg")) {
        int isoYear = 0;
        date.weekNumber(&isoYear);
        return QString::number(isoYear);
    }
    if (token == QLatin1String("HH"))
        return QString::number(time.hour()).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("mm"))
        return QString::number(time.minute()).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("ss"))
        return QString::number(time.second()).rightJustified(2, QLatin1Char('0'));
    if (token == QLatin1String("A"))
        return time.hour() < 12 ? QStringLiteral("AM") : QStringLiteral("PM");
    return QString();
}

// Longest first, so "YYYY" is not read as four separate "Y" tokens matching
// nothing and falling through to literal characters.
const QStringList &tokens()
{
    static const QStringList list = {
        QStringLiteral("YYYY"), QStringLiteral("YY"),
        QStringLiteral("MMMM"), QStringLiteral("MMM"), QStringLiteral("MM"), QStringLiteral("M"),
        QStringLiteral("DD"),   QStringLiteral("Do"),  QStringLiteral("D"),
        QStringLiteral("dddd"), QStringLiteral("ddd"),
        QStringLiteral("gggg"),
        QStringLiteral("WW"),   QStringLiteral("W"),
        QStringLiteral("E"),
        QStringLiteral("HH"),   QStringLiteral("mm"),  QStringLiteral("ss"), QStringLiteral("A"),
    };
    return list;
}

} // namespace

QString format(const QDateTime &when, const QString &pattern)
{
    const QDate date = when.date();
    const QTime time = when.time();

    QString out;
    out.reserve(pattern.size());
    qsizetype i = 0;
    while (i < pattern.size()) {
        if (pattern.at(i) == QLatin1Char('[')) {
            const qsizetype end = pattern.indexOf(QLatin1Char(']'), i + 1);
            if (end < 0) {
                out.append(pattern.mid(i + 1));
                break;
            }
            out.append(pattern.mid(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }

        bool matched = false;
        for (const QString &token : tokens()) {
            if (pattern.mid(i, token.size()) == token) {
                out.append(expand(token, date, time));
                i += token.size();
                matched = true;
                break;
            }
        }
        if (!matched) {
            out.append(pattern.at(i));
            ++i;
        }
    }
    return out;
}

} // namespace noteAppC::MomentFormat
