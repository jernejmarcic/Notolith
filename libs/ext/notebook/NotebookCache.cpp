#include "NotebookCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>

namespace noteAppC::notebook {

QString notebookCacheRoot()
{
    QString base = qEnvironmentVariable("XDG_CACHE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/noteAppC/notebook");
}

QString notebookCacheKey(const QString &language, const QString &source, SandboxTier tier,
                         const QString &prelude, const QString &wrapper)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(language.trimmed().toLower().toUtf8() + '\x1f' + source.toUtf8() + '\x1f'
                + QByteArray::number(int(tier)));
    if (!prelude.isEmpty() || !wrapper.isEmpty())
        hash.addData('\x1f' + prelude.toUtf8() + '\x1f' + wrapper.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

namespace {

QString entryDir(const QString &key)
{
    // Bucketed by the key's first two characters, the way git's own object
    // store is - one flat directory of tens of thousands of cache entries
    // is slow to list for no benefit here.
    return notebookCacheRoot() + QLatin1Char('/') + key.left(2) + QLatin1Char('/') + key;
}

QString kindName(NotebookOutput::Kind kind)
{
    switch (kind) {
    case NotebookOutput::Kind::Text: return QStringLiteral("text");
    case NotebookOutput::Kind::Markdown: return QStringLiteral("markdown");
    case NotebookOutput::Kind::ImagePng: return QStringLiteral("image/png");
    case NotebookOutput::Kind::ImageSvg: return QStringLiteral("image/svg");
    case NotebookOutput::Kind::Error: return QStringLiteral("error");
    case NotebookOutput::Kind::Html: return QStringLiteral("html");
    }
    return QStringLiteral("text");
}

NotebookOutput::Kind kindFromName(const QString &name)
{
    if (name == QLatin1String("markdown"))
        return NotebookOutput::Kind::Markdown;
    if (name == QLatin1String("image/png"))
        return NotebookOutput::Kind::ImagePng;
    if (name == QLatin1String("image/svg"))
        return NotebookOutput::Kind::ImageSvg;
    if (name == QLatin1String("error"))
        return NotebookOutput::Kind::Error;
    if (name == QLatin1String("html"))
        return NotebookOutput::Kind::Html;
    return NotebookOutput::Kind::Text;
}

/// Seeds not yet written (seedNotebookCache). ponytail: kept until their key
/// is read, written or removed, so a session that opens many notebooks and
/// scrolls through none holds their unseen outputs in memory; flush them to
/// disk on a worker if that ever shows in RSS.
QMutex seedMutex;
QHash<QString, std::function<NotebookRunResult()>> &pendingSeeds()
{
    static QHash<QString, std::function<NotebookRunResult()>> seeds;
    return seeds;
}

std::function<NotebookRunResult()> takeSeed(const QString &key)
{
    QMutexLocker lock(&seedMutex);
    return pendingSeeds().take(key);
}

} // namespace

void seedNotebookCache(const QString &key, std::function<NotebookRunResult()> result)
{
    QMutexLocker lock(&seedMutex);
    pendingSeeds().insert(key, std::move(result));
}

NotebookRunResult readNotebookCache(const QString &key)
{
    if (const auto seed = takeSeed(key))
        writeNotebookCache(key, seed());
    NotebookRunResult result;
    const QString dir = entryDir(key);
    QFile metaFile(dir + QStringLiteral("/outputs.json"));
    if (!metaFile.open(QIODevice::ReadOnly))
        return result; // nothing cached: hasRun stays false

    const QJsonObject root = QJsonDocument::fromJson(metaFile.readAll()).object();
    result.hasRun = true;
    result.ok = root.value(QStringLiteral("ok")).toBool(true);
    result.wallMs = qint64(root.value(QStringLiteral("wallMs")).toDouble(-1));
    result.kernelError = root.value(QStringLiteral("kernelError")).toString();
    result.executionCount = root.value(QStringLiteral("count")).toInt();

    int imageIndex = 0;
    for (const QJsonValue &value : root.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject object = value.toObject();
        NotebookOutput output;
        output.kind = kindFromName(object.value(QStringLiteral("kind")).toString());
        output.text = object.value(QStringLiteral("text")).toString();
        output.ename = object.value(QStringLiteral("ename")).toString();
        output.evalue = object.value(QStringLiteral("evalue")).toString();
        output.isStderr = object.value(QStringLiteral("stderr")).toBool();
        output.alt = object.value(QStringLiteral("alt")).toString();
        for (const QJsonValue &line : object.value(QStringLiteral("traceback")).toArray())
            output.traceback << line.toString();
        if (output.kind == NotebookOutput::Kind::ImagePng) {
            QFile image(dir + QStringLiteral("/out-%1.png").arg(imageIndex++));
            if (image.open(QIODevice::ReadOnly))
                output.imageBytes = image.readAll();
        }
        result.outputs.append(output);
    }
    return result;
}

void removeNotebookCache(const QString &key)
{
    takeSeed(key);
    QDir(entryDir(key)).removeRecursively();
}

void writeNotebookCache(const QString &key, NotebookRunResult result)
{
    takeSeed(key); // a run's own output supersedes what the file had
    result.hasRun = true;
    const QString dir = entryDir(key);
    QDir().mkpath(dir);

    QJsonArray outputs;
    int imageIndex = 0;
    for (const NotebookOutput &output : result.outputs) {
        QJsonObject object;
        object.insert(QStringLiteral("kind"), kindName(output.kind));
        object.insert(QStringLiteral("text"), output.text);
        if (!output.ename.isEmpty())
            object.insert(QStringLiteral("ename"), output.ename);
        if (!output.evalue.isEmpty())
            object.insert(QStringLiteral("evalue"), output.evalue);
        if (output.isStderr)
            object.insert(QStringLiteral("stderr"), true);
        if (!output.alt.isEmpty())
            object.insert(QStringLiteral("alt"), output.alt);
        if (!output.traceback.isEmpty())
            object.insert(QStringLiteral("traceback"), QJsonArray::fromStringList(output.traceback));
        if (output.kind == NotebookOutput::Kind::ImagePng && !output.imageBytes.isEmpty()) {
            QFile image(dir + QStringLiteral("/out-%1.png").arg(imageIndex++));
            if (image.open(QIODevice::WriteOnly))
                image.write(output.imageBytes);
        }
        outputs.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("ok"), result.ok);
    root.insert(QStringLiteral("wallMs"), double(result.wallMs));
    if (result.executionCount > 0)
        root.insert(QStringLiteral("count"), result.executionCount);
    if (!result.kernelError.isEmpty())
        root.insert(QStringLiteral("kernelError"), result.kernelError);
    root.insert(QStringLiteral("outputs"), outputs);

    QFile metaFile(dir + QStringLiteral("/outputs.json"));
    if (metaFile.open(QIODevice::WriteOnly))
        metaFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

} // namespace noteAppC::notebook
