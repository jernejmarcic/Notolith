#include "DisplayMap.h"

#include <algorithm>

namespace noteAppC {

namespace {

QList<Range> clipAndMerge(const QList<Range> &ranges, const Range &block)
{
    QList<Range> kept;
    kept.reserve(ranges.size());
    for (const Range &range : ranges) {
        const Range clipped{qMax(range.start, block.start), qMin(range.end, block.end)};
        if (clipped.isValid() && clipped.length() > 0)
            kept.append(clipped);
    }
    std::sort(kept.begin(), kept.end(),
              [](const Range &l, const Range &r) { return l.start < r.start; });

    QList<Range> merged;
    for (const Range &range : std::as_const(kept)) {
        if (!merged.isEmpty() && range.start <= merged.last().end)
            merged.last().end = qMax(merged.last().end, range.end);
        else
            merged.append(range);
    }
    return merged;
}

} // namespace

DisplayMap DisplayMap::identity(const QString &source, const Range &block)
{
    return build(source, block, {});
}

DisplayMap DisplayMap::build(const QString &source, const Range &block,
                             const QList<Range> &hidden,
                             const QList<Substitution> &substitutions)
{
    DisplayMap map;
    map.m_block = {qMax<qsizetype>(0, block.start), qMin<qsizetype>(source.size(), block.end)};
    if (!map.m_block.isValid())
        map.m_block = {0, 0};

    const QList<Range> holes = clipAndMerge(hidden, map.m_block);

    // A visible stretch, cut where a substitution inside it shows other text.
    auto next = std::lower_bound(substitutions.cbegin(), substitutions.cend(), map.m_block.start,
                                 [](const Substitution &s, qsizetype at) { return s.range.start < at; });
    const auto keep = [&](qsizetype from, qsizetype to) {
        while (next != substitutions.cend() && next->range.start < from)
            ++next;
        for (; next != substitutions.cend() && next->range.end <= to; ++next) {
            if (next->range.start > from) {
                map.m_runs.append({map.m_text.size(), from, next->range.start - from, next->range.start - from});
                map.m_text += QStringView(source).mid(from, next->range.start - from);
            }
            map.m_runs.append({map.m_text.size(), next->range.start, next->text.size(),
                               next->range.length(), true});
            map.m_text += next->text;
            from = next->range.end;
        }
        if (to > from) {
            map.m_runs.append({map.m_text.size(), from, to - from, to - from});
            map.m_text += QStringView(source).mid(from, to - from);
        }
    };

    qsizetype cursor = map.m_block.start;
    for (const Range &hole : holes) {
        if (hole.start > cursor)
            keep(cursor, hole.start);
        cursor = qMax(cursor, hole.end);
    }
    if (cursor < map.m_block.end)
        keep(cursor, map.m_block.end);

    return map;
}

bool DisplayMap::hidesAnything() const
{
    qsizetype shown = 0;
    for (const Run &run : m_runs)
        shown += run.sourceLength;
    return shown != m_block.length();
}

qsizetype DisplayMap::toSource(qsizetype displayOffset) const
{
    if (m_runs.isEmpty())
        return m_block.start;
    const qsizetype wanted = qBound<qsizetype>(0, displayOffset, m_text.size());

    // The run whose display span contains `wanted`; the last run also owns the
    // one-past-the-end position.
    auto it = std::upper_bound(m_runs.cbegin(), m_runs.cend(), wanted,
                               [](qsizetype value, const Run &run) {
                                   return value < run.displayStart;
                               });
    const Run &run = *(it == m_runs.cbegin() ? it : it - 1);
    if (run.substituted) // one step: its start, or past all of it
        return run.sourceStart + (wanted == run.displayStart ? 0 : run.sourceLength);
    return run.sourceStart + qMin(wanted - run.displayStart, run.length);
}

qsizetype DisplayMap::toDisplay(qsizetype sourceOffset) const
{
    if (m_runs.isEmpty())
        return 0;
    const qsizetype wanted = qBound(m_block.start, sourceOffset, m_block.end);

    for (const Run &run : m_runs) {
        if (wanted < run.sourceStart)
            return run.displayStart; // inside a hidden stretch: snap forward
        if (wanted <= run.sourceStart + run.sourceLength) {
            if (run.substituted) // inside one: snap forward, as a hidden offset does
                return run.displayStart + (wanted == run.sourceStart ? 0 : run.length);
            return run.displayStart + (wanted - run.sourceStart);
        }
    }
    return m_text.size();
}

bool DisplayMap::isHidden(qsizetype sourceOffset) const
{
    if (sourceOffset < m_block.start || sourceOffset >= m_block.end)
        return false;
    for (const Run &run : m_runs) {
        if (sourceOffset < run.sourceStart)
            return true;
        if (sourceOffset < run.sourceStart + run.sourceLength)
            return false;
    }
    return true;
}

Range DisplayMap::toDisplay(const Range &sourceRange) const
{
    return {toDisplay(sourceRange.start), toDisplay(sourceRange.end)};
}

void DisplayMap::shift(qsizetype delta)
{
    m_block.start += delta;
    m_block.end += delta;
    for (Run &run : m_runs)
        run.sourceStart += delta;
}

} // namespace noteAppC
