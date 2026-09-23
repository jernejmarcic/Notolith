#include "Frontmatter.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>

#include <yaml-cpp/yaml.h>

namespace noteAppC {

namespace {

QString toQString(const YAML::Node &node)
{
    return QString::fromStdString(node.Scalar()).trimmed();
}

/// Every position yaml-cpp reports, in order. It gives a node's start and never
/// its end, so the end of one value is the start of whatever comes next.
void collectMarks(const YAML::Node &node, QList<int> &marks)
{
    marks.append(node.Mark().pos);
    if (node.IsSequence()) {
        for (const auto &item : node)
            collectMarks(item, marks);
    } else if (node.IsMap()) {
        for (const auto &entry : node) {
            collectMarks(entry.first, marks);
            collectMarks(entry.second, marks);
        }
    }
}

/// The source text of one scalar, byte for byte. yaml-cpp's marks are offsets
/// into the UTF-8 it was handed, so the cut is made there and converted back.
///
/// ponytail: a value ends at its line, so a block scalar (`key: |`) keeps only
/// its first line. Walk the indentation when someone writes one in frontmatter.
QString rawOf(const QByteArray &body, const QList<int> &marks, const YAML::Node &node)
{
    if (!node.IsDefined() || node.IsNull())
        return {};
    const int start = node.Mark().pos;
    if (start < 0 || start >= body.size())
        return {};

    const auto next = std::upper_bound(marks.cbegin(), marks.cend(), start);
    int end = next == marks.cend() ? body.size() : *next;
    const int lineEnd = body.indexOf('\n', start);
    if (lineEnd >= 0)
        end = qMin(end, lineEnd);

    // Whatever separated this value from the next one is not part of it.
    while (end > start && (body.at(end - 1) == ',' || body.at(end - 1) == ']'
                           || body.at(end - 1) == '}' || body.at(end - 1) == ' '
                           || body.at(end - 1) == '\t' || body.at(end - 1) == '\r'))
        --end;
    return QString::fromUtf8(body.mid(start, end - start));
}

/// Values and their source text, one entry each, for a scalar or a sequence.
void readValues(const YAML::Node &node, const QByteArray &body, const QList<int> &marks,
                Property &property)
{
    if (node.IsScalar()) {
        const QString value = toQString(node);
        if (!value.isEmpty()) {
            property.values << value;
            property.raw << rawOf(body, marks, node);
        }
    } else if (node.IsSequence()) {
        for (const auto &item : node) {
            if (!item.IsScalar())
                continue;
            property.values << toQString(item);
            property.raw << rawOf(body, marks, item);
        }
    } else if (node.IsMap()) {
        // Obsidian shows nested maps as text; keep the YAML so nothing is lost.
        YAML::Emitter emitter;
        emitter << node;
        property.values << QString::fromStdString(emitter.c_str()).trimmed();
        property.raw << rawOf(body, marks, node);
    }
}

bool isNumber(const QString &value)
{
    bool ok = false;
    value.toDouble(&ok);
    return ok;
}

/// What Obsidian would call this value, asked of the YAML rather than of the
/// string, so `"42"` stays text and a one-item list stays a list.
Property::Type typeOf(const YAML::Node &node)
{
    if (node.IsSequence())
        return Property::Type::List;
    // A null (`key:` with nothing after it) is an empty text property.
    if (!node.IsScalar())
        return Property::Type::Text;
    // yaml-cpp tags a plain scalar "?" and a quoted one "!"; only a plain one
    // can be a number, a checkbox or a date.
    if (node.Tag() == "!")
        return Property::Type::Text;
    return Frontmatter::inferType({toQString(node)});
}

} // namespace

Range Frontmatter::find(const QString &text)
{
    if (!text.startsWith(QLatin1String("---")))
        return {};

    // The opening fence must be a line of its own.
    const qsizetype firstBreak = text.indexOf(QLatin1Char('\n'));
    if (firstBreak < 0)
        return {};
    if (QStringView(text).left(firstBreak).trimmed() != QLatin1String("---"))
        return {};

    qsizetype searchFrom = firstBreak + 1;
    while (searchFrom < text.size()) {
        qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), searchFrom);
        if (lineEnd < 0)
            lineEnd = text.size();
        const QStringView line = QStringView(text).mid(searchFrom, lineEnd - searchFrom);
        if (line.trimmed() == QLatin1String("---"))
            return {0, qMin<qsizetype>(text.size(), lineEnd + 1)};
        searchFrom = lineEnd + 1;
    }
    return {};
}

Property::Type Frontmatter::inferType(const QStringList &values)
{
    if (values.isEmpty())
        return Property::Type::Unknown;
    if (values.size() > 1)
        return Property::Type::List;

    const QString value = values.first();
    if (value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
        return Property::Type::Checkbox;
    if (isNumber(value))
        return Property::Type::Number;
    // Obsidian's date is exactly YYYY-MM-DD, and the length has to say so:
    // QDate::fromString is happy to stop reading at the "T" of a date-time.
    if (value.size() == 10 && QDate::fromString(value, Qt::ISODate).isValid())
        return Property::Type::Date;
    if (QDateTime::fromString(value, Qt::ISODate).isValid())
        return Property::Type::DateTime;
    return Property::Type::Text;
}

std::optional<QString> Frontmatter::scalarFor(const QString &value, Property::Type type)
{
    const QString trimmed = value.trimmed();
    const Property::Type reads = trimmed.isEmpty() ? Property::Type::Text : inferType({trimmed});
    switch (type) {
    case Property::Type::Text:
        break;
    case Property::Type::Number:
    case Property::Type::Date:
    case Property::Type::DateTime:
        if (reads != type)
            return std::nullopt;
        break;
    default:
        return std::nullopt;
    }
    // yaml-cpp decides what needs quoting - "a: b", "#tag", "[x]" - which is
    // the same library that reads it back.
    YAML::Emitter emitter;
    if (type == Property::Type::Text && reads != Property::Type::Text)
        emitter << YAML::DoubleQuoted; // "42" stays text
    emitter << trimmed.toStdString();
    return QString::fromStdString(emitter.c_str());
}

void Frontmatter::parse(const QString &text, const Range &block, Metadata &out)
{
    out.frontmatter = block;
    if (!block.isValid() || block.length() <= 6)
        return;

    // Strip both fences; what is left is the YAML body.
    const qsizetype bodyStart = text.indexOf(QLatin1Char('\n'), block.start) + 1;
    qsizetype bodyEnd = text.lastIndexOf(QLatin1String("---"), block.end - 1);
    if (bodyStart <= 0 || bodyEnd < bodyStart)
        return;
    const QString body = text.mid(bodyStart, bodyEnd - bodyStart);

    YAML::Node root;
    try {
        root = YAML::Load(body.toStdString());
    } catch (const YAML::Exception &error) {
        out.parseWarnings << QStringLiteral("frontmatter: %1").arg(QString::fromStdString(error.what()));
        return;
    }
    if (!root.IsMap())
        return;

    // Marks are byte offsets into what yaml-cpp parsed, and lines are lines of
    // it; both need the body, not the file, to be read back.
    const QByteArray utf8 = body.toUtf8();
    QList<int> marks;
    collectMarks(root, marks);
    std::sort(marks.begin(), marks.end());

    QList<qsizetype> lineStarts{bodyStart};
    for (qsizetype i = 0; i < body.size(); ++i) {
        if (body.at(i) == QLatin1Char('\n'))
            lineStarts.append(bodyStart + i + 1);
    }
    const auto lineStart = [&lineStarts, bodyEnd](int line) {
        return line >= 0 && line < lineStarts.size() ? lineStarts.at(line) : bodyEnd;
    };

    for (const auto &entry : root) {
        if (!entry.first.IsScalar())
            continue;
        const QString key = toQString(entry.first);

        Property property;
        property.key = key;
        property.type = typeOf(entry.second);
        readValues(entry.second, utf8, marks, property);
        // An entry runs to the line the next key starts on, so a list written
        // over several lines keeps all of them.
        property.range = {lineStart(entry.first.Mark().line), bodyEnd};
        if (!out.properties.isEmpty())
            out.properties.last().range.end = property.range.start;
        out.properties.append(property);

        const QStringList &values = property.values;
        const QString lower = key.toLower();
        if (lower == QLatin1String("aliases") || lower == QLatin1String("alias")) {
            out.aliases += values;
        } else if (lower == QLatin1String("tags") || lower == QLatin1String("tag")) {
            for (const QString &value : values) {
                // Frontmatter tags may be written with or without the hash.
                const QString name = value.startsWith(QLatin1Char('#')) ? value.mid(1) : value;
                if (!name.isEmpty())
                    out.tags.append(Tag{name, out.frontmatter});
            }
        } else if (lower == QLatin1String("cssclasses") || lower == QLatin1String("cssclass")) {
            out.cssClasses += values;
        } else if (lower == QLatin1String("id") && !values.isEmpty()) {
            out.id = values.first();
        }
    }
}

} // namespace noteAppC
