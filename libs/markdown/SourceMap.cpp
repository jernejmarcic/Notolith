#include "SourceMap.h"

#include <algorithm>

namespace noteAppC {

SourceMap::SourceMap(const QString &text)
    : m_length(text.size())
{
    // indexOf is vectorised; a loop over at() was a third of a keystroke's
    // cost on a 1 MB note, since the editor rebuilds this on every edit.
    m_lineStarts.append(0);
    const QStringView view(text);
    for (qsizetype i = view.indexOf(u'\n'); i >= 0; i = view.indexOf(u'\n', i + 1))
        m_lineStarts.append(i + 1);
}

void SourceMap::applyEdit(qsizetype at, qsizetype removed, QStringView inserted)
{
    // A line starts at s exactly when s - 1 is a newline. The removed text's
    // newlines are the starts in (at, at + removed]; everything after moves
    // by the change in length; the inserted text brings starts of its own.
    const auto first = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), at);
    const auto last = std::upper_bound(first, m_lineStarts.end(), at + removed);
    const qsizetype delta = inserted.size() - removed;
    for (auto it = last; it != m_lineStarts.end(); ++it)
        *it += delta;

    QList<qsizetype> added;
    for (qsizetype i = inserted.indexOf(u'\n'); i >= 0; i = inserted.indexOf(u'\n', i + 1))
        added.append(at + i + 1);
    const qsizetype index = std::distance(m_lineStarts.begin(), first);
    m_lineStarts.remove(index, std::distance(first, last));
    for (qsizetype i = 0; i < added.size(); ++i)
        m_lineStarts.insert(index + i, added.at(i));
    m_length += delta;
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
