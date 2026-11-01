#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace noteAppC {

/// The picture in `bytes`, null when there is none. An SVG (found by its content, whatever the file is
/// called) goes through safeSvg() (libs/markdown/SafeSvg.h) so it cannot pull in files of its own; any
/// other format is decoded as it is. Every place that decodes a picture a vault, a book or a kernel
/// supplied uses this, not QImage(path) / QPixmap(path) / QSvgRenderer(path) / loadFromData().
/// `autoTransform` turns a photo upright by its EXIF orientation.
QImage decodeImage(const QByteArray &bytes, bool autoTransform = false);

/// decodeImage() of the file at `path`. `maxWidth` > 0 scales a wider picture down to it (a raster
/// file is decoded at that size, not scaled after).
QImage readImage(const QString &path, int maxWidth = 0, bool autoTransform = false);

} // namespace noteAppC
