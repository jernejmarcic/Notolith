#include "ImageLoad.h"

#include <QBuffer>
#include <QFile>
#include <QImageReader>

#include "SafeSvg.h"

namespace noteAppC {

namespace {
/// An SVG starts with `<` (after a byte order mark and blanks): the prolog, a comment, a doctype or the
/// root. safeSvg() parses it strictly and refuses what is not XML.
bool looksLikeSVG(const QByteArray &b)
{
    qsizetype i = b.startsWith("\xef\xbb\xbf") ? 3 : 0;
    while (i < b.size() && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r'))
        ++i;
    return i < b.size() && b[i] == '<';
}

/// A reader that tries `format` and nothing else.
QImage readAs(QIODevice *device, const QByteArray &format, bool autoTransform, int maxWidth = 0)
{
    QImageReader reader(device, format);
    reader.setAutoDetectImageFormat(false);
    reader.setAutoTransform(autoTransform);
    if (maxWidth > 0) {
        const QSize size = reader.size();
        if (size.width() > maxWidth)
            reader.setScaledSize(size.scaled(maxWidth, size.height(), Qt::KeepAspectRatio));
    }
    return reader.read();
}
} // namespace

QByteArray imageFormat(const QByteArray &b)
{
    if (b.startsWith("\x89PNG\r\n\x1a\n"))
        return "png";
    if (b.startsWith("\xff\xd8\xff"))
        return "jpeg";
    if (b.startsWith("GIF87a") || b.startsWith("GIF89a"))
        return "gif";
    if (b.startsWith("BM"))
        return "bmp";
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP")
        return "webp";
    if (b.size() >= 12 && b.mid(4, 4) == "ftyp" && (b.mid(8, 4) == "avif" || b.mid(8, 4) == "avis"))
        return "avif";
    if (looksLikeSVG(b))
        return "svg";
    return {};
}

bool isImageSuffix(const QString &suffix)
{
    static const QStringList images = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                       QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("svg"),
                                       QStringLiteral("webp"), QStringLiteral("avif")};
    return images.contains(suffix);
}

bool isPictureFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && !imageFormat(file.peek(512)).isEmpty();
}

QImage decodeImage(const QByteArray &bytes, bool autoTransform)
{
    const QByteArray format = imageFormat(bytes);
    if (format.isEmpty())
        return {};
    if (format == "svg") {
        const QByteArray safe = safeSvg(bytes);
        return safe.isEmpty() ? QImage() : QImage::fromData(safe, "svg");
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return readAs(&buffer, format, autoTransform);
}

QImage readImage(const QString &path, int maxWidth, bool autoTransform)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray format = imageFormat(file.peek(512));
    if (format.isEmpty())
        return {};
    if (format != "svg") // decoded from the file, at the size wanted
        return readAs(&file, format, autoTransform, maxWidth);
    QImage image = decodeImage(file.readAll(), autoTransform);
    if (maxWidth > 0 && image.width() > maxWidth)
        image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    return image;
}

} // namespace noteAppC
