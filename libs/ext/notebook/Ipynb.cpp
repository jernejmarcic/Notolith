#include "Ipynb.h"

#include "NotebookOutputs.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <vector>

namespace noteAppC::notebook {

namespace {

const QString kCellMarker = QStringLiteral("<!-- cell -->");

/// nbformat stores text as one string or a list of strings, to be joined.
QString joined(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    QString text;
    for (const QJsonValue &part : value.toArray())
        text += part.toString();
    return text;
}

/// A cell's text as the list of lines nbformat writes (each but the last ends in "\n").
QJsonArray linesOf(const QString &text)
{
    QJsonArray lines;
    qsizetype start = 0;
    while (start < text.size()) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), start);
        const qsizetype end = newline < 0 ? text.size() : newline + 1;
        lines.append(text.mid(start, end - start));
        start = end;
    }
    return lines;
}

struct Cell {
    enum class Type { Markdown, Code, Raw };
    Type type = Type::Markdown;
    QString source;
};

QString typeName(Cell::Type type)
{
    return type == Cell::Type::Code ? QStringLiteral("code")
           : type == Cell::Type::Raw ? QStringLiteral("raw")
                                     : QStringLiteral("markdown");
}

/// The cells of a notebook that this view can show; others (a future cell type) are left out.
QList<Cell> cellsOf(const QJsonObject &root)
{
    QList<Cell> cells;
    for (const QJsonValue &value : root.value(QStringLiteral("cells")).toArray()) {
        const QJsonObject cell = value.toObject();
        const QString type = cell.value(QStringLiteral("cell_type")).toString();
        if (type != QLatin1String("markdown") && type != QLatin1String("code") && type != QLatin1String("raw"))
            continue;
        cells.append({type == QLatin1String("code")  ? Cell::Type::Code
                      : type == QLatin1String("raw") ? Cell::Type::Raw
                                                     : Cell::Type::Markdown,
                      joined(cell.value(QStringLiteral("source")))});
    }
    return cells;
}

/// A fence long enough that nothing inside the code closes it early.
QString fenceFor(const QString &code)
{
    qsizetype longest = 0;
    qsizetype run = 0;
    for (const QChar c : code) {
        run = c == QLatin1Char('`') ? run + 1 : 0;
        longest = qMax(longest, run);
    }
    return QString(qMax<qsizetype>(3, longest + 1), QLatin1Char('`'));
}

QString markdownOf(const QList<Cell> &cells, const QString &language)
{
    QString text;
    Cell::Type previous = Cell::Type::Code;
    for (qsizetype i = 0; i < cells.size(); ++i) {
        const Cell &cell = cells.at(i);
        if (i > 0) {
            text += QStringLiteral("\n\n");
            if (cell.type == Cell::Type::Markdown && previous == Cell::Type::Markdown)
                text += kCellMarker + QStringLiteral("\n\n");
        }
        if (cell.type == Cell::Type::Markdown) {
            text += cell.source;
        } else {
            const QString fence = fenceFor(cell.source);
            text += fence + (cell.type == Cell::Type::Raw ? QStringLiteral("raw") : language) + QLatin1Char('\n')
                    + cell.source + QLatin1Char('\n') + fence;
        }
        previous = cell.type;
    }
    return cells.isEmpty() ? text : text + QLatin1Char('\n');
}

/// The inverse of markdownOf for any text: fences in the notebook's language (or
/// `raw`) are code cells, whatever lies between them is markdown, and every
/// other fence stays inside the markdown it is part of. A same-language fence
/// inside a markdown cell therefore becomes a code cell (jupytext's rule too).
QList<Cell> cellsFromMarkdown(const QString &markdown, const QString &language)
{
    static const QRegularExpression opening(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,})[ \t]*([^\s`]*)[^`]*$)"));
    QStringList lines = markdown.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast(); // the newline that ends the text

    QList<Cell> cells;
    QStringList buffer;
    bool afterCell = false; // a cell or marker came before: its separating blank line leads the buffer
    const auto flush = [&] {
        if (afterCell && !buffer.isEmpty() && buffer.first().isEmpty())
            buffer.removeFirst();
        if (!buffer.isEmpty() && buffer.last().isEmpty())
            buffer.removeLast();
        const bool blank = std::all_of(buffer.cbegin(), buffer.cend(), [](const QString &line) { return line.isEmpty(); });
        if (!blank)
            cells.append({Cell::Type::Markdown, buffer.join(QLatin1Char('\n'))});
        buffer.clear();
    };

    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = opening.match(lines.at(i));
        if (match.hasMatch()) {
            const QString fence = match.captured(1);
            const QString info = match.captured(2).toLower();
            // The closing fence: the same character, at least as long, nothing after it.
            qsizetype close = lines.size();
            const QRegularExpression closing(QStringLiteral("^ {0,3}%1{%2,}[ \\t]*$")
                                                 .arg(QRegularExpression::escape(fence.left(1)))
                                                 .arg(fence.size()));
            for (qsizetype j = i + 1; j < lines.size(); ++j) {
                if (closing.match(lines.at(j)).hasMatch()) {
                    close = j;
                    break;
                }
            }
            if (info == language || info == QLatin1String("raw")) {
                flush();
                cells.append({info == QLatin1String("raw") ? Cell::Type::Raw : Cell::Type::Code,
                              QStringList(lines.mid(i + 1, close - i - 1)).join(QLatin1Char('\n'))});
                afterCell = true;
            } else {
                for (qsizetype j = i; j <= close && j < lines.size(); ++j)
                    buffer << lines.at(j);
            }
            i = close;
            continue;
        }
        if (lines.at(i) == kCellMarker) {
            flush();
            afterCell = true;
            continue;
        }
        buffer << lines.at(i);
    }
    flush();
    return cells;
}

QString newCellID()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(8);
}

QJsonObject newCell(const Cell &cell, int nbformatMinor)
{
    QJsonObject object;
    object.insert(QStringLiteral("cell_type"), typeName(cell.type));
    object.insert(QStringLiteral("metadata"), QJsonObject());
    object.insert(QStringLiteral("source"), linesOf(cell.source));
    if (cell.type == Cell::Type::Code) {
        object.insert(QStringLiteral("execution_count"), QJsonValue::Null);
        object.insert(QStringLiteral("outputs"), QJsonArray());
    }
    if (nbformatMinor >= 5)
        object.insert(QStringLiteral("id"), newCellID());
    return object;
}

// --- stored outputs -> what the editor shows ----------------------------------

bool hasContent(const NotebookOutput &output)
{
    return !output.text.isEmpty() || !output.imageBytes.isEmpty();
}

/// A display bundle's values with nbformat's "list of lines" joined into strings.
QJsonObject flattened(const QJsonObject &data)
{
    QJsonObject out;
    for (auto it = data.begin(); it != data.end(); ++it)
        out.insert(it.key(), it.value().isArray() ? QJsonValue(joined(it.value())) : it.value());
    return out;
}

NotebookRunResult resultOfCell(const QJsonObject &cell)
{
    NotebookRunResult result;
    result.hasRun = true;
    result.executionCount = qMax(0, cell.value(QStringLiteral("execution_count")).toInt(0));
    for (const QJsonValue &value : cell.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject stored = value.toObject();
        const QString type = stored.value(QStringLiteral("output_type")).toString();
        if (type == QLatin1String("stream")) {
            const bool isStderr = stored.value(QStringLiteral("name")).toString() == QLatin1String("stderr");
            const QString text = stripAnsi(joined(stored.value(QStringLiteral("text"))));
            if (!result.outputs.isEmpty() && result.outputs.last().kind == NotebookOutput::Kind::Text
                && result.outputs.last().isStderr == isStderr && result.outputs.last().displayID.isEmpty()) {
                result.outputs.last().text = resolveCarriageReturns(result.outputs.last().text + text);
            } else {
                NotebookOutput output;
                output.kind = NotebookOutput::Kind::Text;
                output.isStderr = isStderr;
                output.text = resolveCarriageReturns(text);
                result.outputs.append(output);
            }
        } else if (type == QLatin1String("error")) {
            NotebookOutput output;
            output.kind = NotebookOutput::Kind::Error;
            output.ename = stored.value(QStringLiteral("ename")).toString();
            output.evalue = stored.value(QStringLiteral("evalue")).toString();
            for (const QJsonValue &line : stored.value(QStringLiteral("traceback")).toArray())
                output.traceback << stripAnsi(line.toString());
            result.ok = false;
            result.outputs.append(output);
        } else if (type == QLatin1String("execute_result") || type == QLatin1String("display_data")) {
            NotebookOutput output = outputFromMimeBundle(flattened(stored.value(QStringLiteral("data")).toObject()));
            if (hasContent(output))
                result.outputs.append(output);
        }
    }
    return result;
}

// --- JSON in nbformat's style ---------------------------------------------------

void appendString(QByteArray &out, const QString &text)
{
    out += '"';
    for (const QChar c : text) {
        const ushort u = c.unicode();
        switch (u) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (u < 0x20)
                out += QByteArray("\\u") + QByteArray::number(u, 16).rightJustified(4, '0');
            else
                out += QString(c).toUtf8();
        }
    }
    out += '"';
}

void appendNumber(QByteArray &out, double number)
{
    // ponytail: a double cannot tell 1.0 from 1, so a float with no fraction is written as an integer, and an
    // integer beyond 2^53 loses digits; nothing nbformat itself stores is either.
    if (number == std::floor(number) && std::abs(number) < 9.007199254740992e15)
        out += QByteArray::number(qint64(number));
    else
        out += QLocale::c().toString(number, 'g', QLocale::FloatingPointShortest).toUtf8();
}

void appendValue(QByteArray &out, const QJsonValue &value, int depth)
{
    const QByteArray indent(depth + 1, ' ');
    const QByteArray closing(depth, ' ');
    switch (value.type()) {
    case QJsonValue::Null: out += "null"; break;
    case QJsonValue::Bool: out += value.toBool() ? "true" : "false"; break;
    case QJsonValue::Double: appendNumber(out, value.toDouble()); break;
    case QJsonValue::String: appendString(out, value.toString()); break;
    case QJsonValue::Array: {
        const QJsonArray array = value.toArray();
        if (array.isEmpty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (qsizetype i = 0; i < array.size(); ++i) {
            out += indent;
            appendValue(out, array.at(i), depth + 1);
            out += i + 1 < array.size() ? ",\n" : "\n";
        }
        out += closing + "]";
        break;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            out += "{}";
            break;
        }
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        out += "{\n";
        for (qsizetype i = 0; i < keys.size(); ++i) {
            out += indent;
            appendString(out, keys.at(i));
            out += ": ";
            appendValue(out, object.value(keys.at(i)), depth + 1);
            out += i + 1 < keys.size() ? ",\n" : "\n";
        }
        out += closing + "}";
        break;
    }
    case QJsonValue::Undefined: out += "null"; break;
    }
}

} // namespace

bool isIpynbPath(const QString &path)
{
    return path.endsWith(QLatin1String(".ipynb"), Qt::CaseInsensitive);
}

QByteArray dumpIpynbJSON(const QJsonObject &root)
{
    QByteArray out;
    appendValue(out, root, 0);
    return out + '\n';
}

bool parseIpynb(const QByteArray &bytes, IpynbDocument *document, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (!json.isObject())
        return fail(QStringLiteral("not a notebook: %1").arg(parseError.errorString()));
    const QJsonObject root = json.object();
    const int format = root.value(QStringLiteral("nbformat")).toInt(0);
    if (format != 4)
        return fail(QStringLiteral("nbformat %1 is not supported (only 4)").arg(format));
    if (!root.value(QStringLiteral("cells")).isArray())
        return fail(QStringLiteral("not a notebook: it has no cells"));

    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    QString language = metadata.value(QStringLiteral("kernelspec")).toObject().value(QStringLiteral("language")).toString();
    if (language.isEmpty())
        language = metadata.value(QStringLiteral("language_info")).toObject().value(QStringLiteral("name")).toString();
    document->root = root;
    document->language = language.trimmed().isEmpty() ? QStringLiteral("python") : language.trimmed().toLower();
    document->nbformatMinor = root.value(QStringLiteral("nbformat_minor")).toInt(0);
    return true;
}

QString ipynbMarkdown(const IpynbDocument &document)
{
    return markdownOf(cellsOf(document.root), document.language);
}

// Lazily (seedNotebookCache): decoding every stored PNG and writing it out at each read was 230 ms of a 350 ms
// open for a notebook of 40 plots, on the GUI thread; now only the blocks laid out pay, when they look themselves up.
void seedIpynbOutputs(const IpynbDocument &document, SandboxTier tier)
{
    for (const QJsonValue &value : document.root.value(QStringLiteral("cells")).toArray()) {
        const QJsonObject cell = value.toObject();
        if (cell.value(QStringLiteral("cell_type")).toString() != QLatin1String("code"))
            continue;
        const bool ran = !cell.value(QStringLiteral("outputs")).toArray().isEmpty()
                         || !cell.value(QStringLiteral("execution_count")).isNull();
        if (!ran)
            continue; // never run: nothing the file can say about it
        // The editor looks a block up by its text between the fences, minus the newline that ends it and any
        // blank lines that open it.
        QString source = joined(cell.value(QStringLiteral("source")));
        while (source.startsWith(QLatin1Char('\n')))
            source.remove(0, 1);
        seedNotebookCache(notebookCacheKey(document.language, source, tier), [cell] { return resultOfCell(cell); });
    }
}

bool readIpynb(const QByteArray &bytes, QString *markdown, QString *error)
{
    IpynbDocument document;
    if (!parseIpynb(bytes, &document, error))
        return false;
    seedIpynbOutputs(document);
    *markdown = ipynbMarkdown(document);
    return true;
}

QByteArray mergeIpynb(const QByteArray &fileBytes, const QString &markdown, QString *error)
{
    IpynbDocument document;
    if (!parseIpynb(fileBytes, &document, error))
        return {};
    if (markdown == ipynbMarkdown(document))
        return fileBytes; // not edited: the file's own bytes

    // The cells the file has, as objects, in the order and with the text this view shows them.
    QList<QJsonObject> originals;
    for (const QJsonValue &value : document.root.value(QStringLiteral("cells")).toArray()) {
        const QString type = value.toObject().value(QStringLiteral("cell_type")).toString();
        if (type == QLatin1String("markdown") || type == QLatin1String("code") || type == QLatin1String("raw"))
            originals.append(value.toObject());
    }
    const auto typeOf = [](const QJsonObject &cell) { return cell.value(QStringLiteral("cell_type")).toString(); };
    const auto sourceOf = [](const QJsonObject &cell) { return joined(cell.value(QStringLiteral("source"))); };

    const QList<Cell> shown = cellsFromMarkdown(markdown, document.language);
    const qsizetype n = shown.size();
    const qsizetype m = originals.size();
    const auto same = [&](qsizetype i, qsizetype k) {
        return typeOf(originals.at(k)) == typeName(shown.at(i).type) && sourceOf(originals.at(k)) == shown.at(i).source;
    };
    // The cells that did not change, found as the longest run of them in order (a cell inserted or deleted
    // in the middle does not make every cell after it look edited). ponytail: a table n by m, so a notebook of
    // thousands of cells is paired by position instead; add a diff library if that ever matters.
    const bool tooBig = n * m > 4'000'000;
    std::vector<std::vector<int>> run(tooBig ? 0 : size_t(n) + 1, std::vector<int>(size_t(m) + 1, 0));
    for (qsizetype i = tooBig ? -1 : n - 1; i >= 0; --i) {
        for (qsizetype k = m - 1; k >= 0; --k)
            run[size_t(i)][size_t(k)] = same(i, k) ? run[size_t(i) + 1][size_t(k) + 1] + 1
                                                   : std::max(run[size_t(i) + 1][size_t(k)], run[size_t(i)][size_t(k) + 1]);
    }
    // Between two unchanged cells, what is left on each side pairs up in order where the types agree: an edited
    // cell keeps its id, metadata and outputs. The rest is new (or gone).
    QJsonArray rebuilt;
    const auto pairGap = [&](qsizetype fromNew, qsizetype toNew, qsizetype fromOld, qsizetype toOld) {
        qsizetype k = fromOld;
        for (qsizetype i = fromNew; i < toNew; ++i) {
            if (k < toOld && typeOf(originals.at(k)) == typeName(shown.at(i).type)) {
                QJsonObject edited = originals.at(k++);
                edited.insert(QStringLiteral("source"), linesOf(shown.at(i).source));
                rebuilt.append(edited);
            } else {
                rebuilt.append(newCell(shown.at(i), document.nbformatMinor));
            }
        }
    };
    qsizetype i = 0;
    qsizetype k = 0;
    while (!tooBig && i < n && k < m) {
        if (same(i, k) && run[size_t(i)][size_t(k)] == run[size_t(i) + 1][size_t(k) + 1] + 1) {
            rebuilt.append(originals.at(k));
            ++i;
            ++k;
            continue;
        }
        // The gap ends at the next unchanged pair: walk both sides until one is found.
        qsizetype gapNew = i;
        qsizetype gapOld = k;
        while (gapNew < n && gapOld < m
               && !(same(gapNew, gapOld) && run[size_t(gapNew)][size_t(gapOld)] == run[size_t(gapNew) + 1][size_t(gapOld) + 1] + 1)) {
            if (run[size_t(gapNew) + 1][size_t(gapOld)] >= run[size_t(gapNew)][size_t(gapOld) + 1])
                ++gapNew;
            else
                ++gapOld;
        }
        pairGap(i, gapNew, k, gapOld);
        i = gapNew;
        k = gapOld;
    }
    pairGap(i, n, k, m);

    if (rebuilt == document.root.value(QStringLiteral("cells")).toArray())
        return fileBytes; // the text differs but its cells are the file's own

    QJsonObject root = document.root;
    root.insert(QStringLiteral("cells"), rebuilt);
    return dumpIpynbJSON(root);
}

} // namespace noteAppC::notebook
