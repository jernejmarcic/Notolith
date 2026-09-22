#include "Fuzzy.h"

namespace noteAppC::Fuzzy {

namespace {

constexpr int kConsecutiveBonus = 12;
constexpr int kWordStartBonus = 10;
constexpr int kPrefixBonus = 16;
constexpr int kGapPenalty = 2;
constexpr int kMaxGap = 8;

bool isBoundary(const QString &text, qsizetype at)
{
    if (at == 0)
        return true;
    const QChar previous = text.at(at - 1);
    if (previous.isSpace() || previous == QLatin1Char('/') || previous == QLatin1Char('-')
        || previous == QLatin1Char('_') || previous == QLatin1Char('.'))
        return true;
    // A capital after a lower-case letter starts a word in camelCase.
    return previous.isLower() && text.at(at).isUpper();
}

} // namespace

Match score(const QString &query, const QString &text)
{
    Match match;
    if (query.isEmpty()) {
        match.matched = true;
        return match;
    }
    if (text.isEmpty())
        return match;

    const QString foldedQuery = query.toCaseFolded();
    const QString foldedText = text.toCaseFolded();

    qsizetype at = 0;
    qsizetype previousHit = -1;
    for (const QChar wanted : foldedQuery) {
        const qsizetype hit = foldedText.indexOf(wanted, at);
        if (hit < 0)
            return match;

        match.positions.append(hit);
        if (hit == 0) {
            match.score += kPrefixBonus;
        } else if (previousHit >= 0 && hit == previousHit + 1) {
            // An unbroken run is the strongest signal; it must outweigh a
            // scattered match that happens to land on word starts.
            match.score += kConsecutiveBonus;
        } else {
            match.score -= int(qMin<qsizetype>(hit - at, kMaxGap)) * kGapPenalty;
            if (isBoundary(text, hit))
                match.score += kWordStartBonus;
        }

        previousHit = hit;
        at = hit + 1;
    }

    // Shorter candidates win ties: "Home" beats "Homework" for "home".
    match.score += int(qMax<qsizetype>(0, 32 - text.size()));
    match.matched = true;
    return match;
}

} // namespace noteAppC::Fuzzy
