#include "SourceMap.h"

#include <algorithm>

namespace noteAppC {

SourceMap::SourceMap(const QString &text)
    : m_length(text.size())
{
    m_lineStarts.append(0);
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\n'))
            m_lineStarts.append(i + 1);
    }
}

qsizetype SourceMap::lineStart(qsizetype line) const
{
    if (m_lineStarts.isEmpty())
        return 0;
    if (line < 0)
        return 0;
    if (line >= m_lineStarts.size())
        return m_length;
    return m_lineStarts.at(line);
}

qsizetype SourceMap::lineEnd(qsizetype line) const
{
    if (line + 1 < m_lineStarts.size())
        return m_lineStarts.at(line + 1) - 1; // drop the newline itself
    return m_length;
}

qsizetype SourceMap::offset(qsizetype line, qsizetype column) const
{
    return qBound(lineStart(line), lineStart(line) + qMax<qsizetype>(0, column), lineEnd(line));
}

qsizetype SourceMap::lineAt(qsizetype offset) const
{
    if (m_lineStarts.isEmpty() || offset <= 0)
        return 0;
    const auto it = std::upper_bound(m_lineStarts.cbegin(), m_lineStarts.cend(), offset);
    return qMax<qsizetype>(0, std::distance(m_lineStarts.cbegin(), it) - 1);
}

Range SourceMap::range(qsizetype startLine, qsizetype startColumn,
                       qsizetype endLine, qsizetype endColumn) const
{
    if (startLine < 0 || endLine < 0)
        return {};
    // md4qt's end column is inclusive.
    return {offset(startLine, startColumn), qMin(m_length, offset(endLine, endColumn + 1))};
}

} // namespace noteAppC
