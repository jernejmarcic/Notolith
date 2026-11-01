#include "ImageLoad.h"

#include <QBuffer>
#include <QFile>
#include <QImageReader>

#include "SafeSvg.h"

namespace noteAppC {

namespace {
/// A format Qt decodes by itself: told by its first bytes, so the common case does not load every image
/// plugin to ask which format it is.
bool isRasterMagic(const QByteArray &b)
{
    return b.startsWith("\x89PNG") || b.startsWith("\xff\xd8\xff") || b.startsWith("GIF8") || b.startsWith("BM")
        || (b.size() > 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP");
}
} // namespace

QImage decodeImage(const QByteArray &bytes, bool autoTransform)
{
    if (bytes.isEmpty())
        return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    // A format told by its first bytes needs no asking; any other is asked, and cleaned if it is an SVG.
    if (!isRasterMagic(bytes)) {
        const QByteArray format = QImageReader(&buffer).format();
        if (format == "svg" || format == "svgz") {
            const QByteArray safe = format == "svg" ? safeSvg(bytes) : QByteArray();
            return safe.isEmpty() ? QImage() : QImage::fromData(safe, "svg");
        }
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(autoTransform);
    return reader.read();
}

QImage readImage(const QString &path, int maxWidth, bool autoTransform)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (isRasterMagic(file.peek(16))) {
        QImageReader reader(&file);
        reader.setAutoTransform(autoTransform);
        const QSize size = reader.size();
        if (maxWidth > 0 && size.width() > maxWidth)
            reader.setScaledSize(size.scaled(maxWidth, size.height(), Qt::KeepAspectRatio));
        return reader.read();
    }
    QImage image = decodeImage(file.readAll(), autoTransform);
    if (maxWidth > 0 && image.width() > maxWidth)
        image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    return image;
}

} // namespace noteAppC
