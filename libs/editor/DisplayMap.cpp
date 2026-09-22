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
                             const QList<Range> &hidden)
{
    DisplayMap map;
    map.m_block = {qMax<qsizetype>(0, block.start), qMin<qsizetype>(source.size(), block.end)};
    if (!map.m_block.isValid())
        map.m_block = {0, 0};

    const QList<Range> holes = clipAndMerge(hidden, map.m_block);

    qsizetype cursor = map.m_block.start;
    for (const Range &hole : holes) {
        if (hole.start > cursor) {
            map.m_runs.append({map.m_text.size(), cursor, hole.start - cursor});
            map.m_text += QStringView(source).mid(cursor, hole.start - cursor);
        }
        cursor = qMax(cursor, hole.end);
    }
    if (cursor < map.m_block.end) {
        map.m_runs.append({map.m_text.size(), cursor, map.m_block.end - cursor});
        map.m_text += QStringView(source).mid(cursor, map.m_block.end - cursor);
    }

    return map;
}

bool DisplayMap::hidesAnything() const
{
    return m_text.size() != m_block.length();
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
        if (wanted <= run.sourceStart + run.length)
            return run.displayStart + (wanted - run.sourceStart);
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
        if (sourceOffset < run.sourceStart + run.length)
            return false;
    }
    return true;
}

Range DisplayMap::toDisplay(const Range &sourceRange) const
{
    return {toDisplay(sourceRange.start), toDisplay(sourceRange.end)};
}

} // namespace noteAppC
