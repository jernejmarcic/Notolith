#include "InkTextBoxes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSemaphore>

#include <zlib.h>

namespace noteAppC {

namespace {

// inkcore's limits are for opening one file (kMaxInflatedBytes, 64 MiB, which holds ~1 GB of parsed JSON at its worst:
// measured). The index reads every file of a vault on the thread pool, so it takes kInkIndexMaxBytes and two at a time.
// ponytail: ceiling ~2 x the parse of a 16 MiB file (a few hundred MB); raise the semaphore if indexing ink is ever the slow part.
constexpr qint64 kMaxInflatedBytes = kInkIndexMaxBytes;
// What one file may add to the index; far above any page of text boxes.
constexpr qsizetype kMaxTextChars = 1 << 20;

// The same call as inkcore's inflateGzip: a gzip member, bounded, damaged or truncated is refused.
bool inflateGzip(const QByteArray &in, QByteArray *out)
{
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK)
        return false;
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    stream.avail_in = uInt(in.size());
    char buffer[1 << 16];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        rc = inflate(&stream, Z_NO_FLUSH);
        const qint64 produced = qint64(sizeof buffer - stream.avail_out);
        if ((rc != Z_OK && rc != Z_STREAM_END) || out->size() + produced > kMaxInflatedBytes) {
            inflateEnd(&stream);
            return false;
        }
        out->append(buffer, int(produced));
    }
    const bool leftover = stream.avail_in != 0; // trailing bytes after the member: inkcore refuses them too
    inflateEnd(&stream);
    return !leftover;
}

} // namespace

std::optional<QString> inkTextBoxes(const QByteArray &bytes)
{
    // The byte-level defence for any caller; the Indexer refuses a bigger file before it reads it.
    if (bytes.size() > kMaxInflatedBytes)
        return std::nullopt;
    static QSemaphore parsing(2);
    parsing.acquire();
    const QSemaphoreReleaser releaser(parsing);
    QByteArray json = bytes;
    if (bytes.startsWith("\x1f\x8b")) {
        json.clear();
        if (!inflateGzip(bytes, &json))
            return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject())
        return std::nullopt;
    // The key names are the .ink v1 schema's (docs/INK.md, InkFormat.h); a newer version is read the same way, unknown keys ignored.
    const QJsonObject top = document.object();
    if (top.value(QLatin1String("format")).toString() != QLatin1String("notolith-ink"))
        return std::nullopt;
    const QJsonValue version = top.value(QLatin1String("version"));
    if (!version.isDouble() || !(version.toDouble() >= 1))
        return std::nullopt;
    QString text;
    for (const QJsonValue &item : top.value(QLatin1String("items")).toArray()) {
        const QJsonObject object = item.toObject();
        const QJsonValue markdown = object.value(QLatin1String("markdown"));
        if (object.value(QLatin1String("type")).toString() != QLatin1String("textbox") || !markdown.isString())
            continue;
        if (!text.isEmpty())
            text += QLatin1String("\n\n");
        text += markdown.toString();
        if (text.size() >= kMaxTextChars) {
            text.truncate(kMaxTextChars);
            if (text.back().isHighSurrogate())
                text.chop(1); // not half an emoji
            break;
        }
    }
    return text;
}

} // namespace noteAppC
