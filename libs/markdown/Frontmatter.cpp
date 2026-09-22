#include "Frontmatter.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>

#include <yaml-cpp/yaml.h>

namespace noteAppC {

namespace {

QString toQString(const YAML::Node &node)
{
    return QString::fromStdString(node.Scalar()).trimmed();
}

QStringList scalarOrSequence(const YAML::Node &node)
{
    QStringList values;
    if (node.IsScalar()) {
        const QString value = toQString(node);
        if (!value.isEmpty())
            values << value;
    } else if (node.IsSequence()) {
        for (const auto &item : node) {
            if (item.IsScalar())
                values << toQString(item);
        }
    } else if (node.IsMap()) {
        // Obsidian shows nested maps as text; keep the YAML so nothing is lost.
        YAML::Emitter emitter;
        emitter << node;
        values << QString::fromStdString(emitter.c_str()).trimmed();
    }
    return values;
}

bool isNumber(const QString &value)
{
    bool ok = false;
    value.toDouble(&ok);
    return ok;
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
    if (QDate::fromString(value, Qt::ISODate).isValid())
        return Property::Type::Date;
    if (QDateTime::fromString(value, Qt::ISODate).isValid())
        return Property::Type::DateTime;
    return Property::Type::Text;
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

    for (const auto &entry : root) {
        if (!entry.first.IsScalar())
            continue;
        const QString key = toQString(entry.first);
        const QStringList values = scalarOrSequence(entry.second);

        Property property;
        property.key = key;
        property.values = values;
        property.type = inferType(values);
        out.properties.append(property);

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
