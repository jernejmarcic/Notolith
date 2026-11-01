// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace noteAppC {

/// The picture format `bytes` start like, by the bytes and never by a file name: png, jpeg, gif, bmp,
/// webp, avif or svg (Obsidian's set), empty for anything else. Only these are ever decoded, so a file
/// that an installed Qt image plugin could read (eps, psd, tiff, raw, pdf...) never reaches that plugin.
/// remoteimage::sniffFormat (libs/remoteimage/ImageWire.h) is the narrower copy for remote images; it stays apart
/// so the sandboxed decode helper does not link this library.
QByteArray imageFormat(const QByteArray &bytes);

/// Whether a file suffix (lower case, no dot) is a picture: Obsidian's png, jpg, jpeg, gif, bmp, svg, webp,
/// avif. Never asks Qt, so no image plugin is loaded to answer.
bool isImageSuffix(const QString &suffix);

/// Whether the file at `path` starts like a picture of that set (reads a few bytes, decodes nothing).
bool isPictureFile(const QString &path);

/// The picture in `bytes`, null when there is none. An SVG goes through safeSvg()
/// (libs/markdown/SafeSvg.h) so it cannot pull in files of its own; any other format is decoded with
/// its reader told which format it is (no autodetection, so no other plugin is asked). Every place that decodes a picture a vault, a book or a kernel
/// supplied uses this, not QImage(path) / QPixmap(path) / QSvgRenderer(path) / loadFromData().
/// `autoTransform` turns a photo upright by its EXIF orientation.
QImage decodeImage(const QByteArray &bytes, bool autoTransform = false);

/// decodeImage() of the file at `path`. `maxWidth` > 0 scales a wider picture down to it (a raster
/// file is decoded at that size, not scaled after).
QImage readImage(const QString &path, int maxWidth = 0, bool autoTransform = false);

} // namespace noteAppC
