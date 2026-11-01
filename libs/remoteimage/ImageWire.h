// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

// What passes between the app and notolith-imagedecode, the helper that decodes
// a remote image. The helper reads the image's bytes on stdin and writes one
// reply on stdout: a 20-byte header (all numbers little-endian) and the pixels.
//
//   "NIMG"  status  width  height  format  pixels...
//
// status 0 is success; any other value is an error and no pixels follow. The
// pixels are width*height*4 bytes, ARGB32 premultiplied. The app trusts none of
// it: validateReply checks the header against the limits and the byte count
// against the dimensions before a QImage is built.

#include <QByteArray>
#include <QImage>
#include <QString>

namespace noteAppC::remoteimage {

enum class ImageFormat : quint32 { None = 0, PNG = 1, JPEG = 2, GIF = 3, WebP = 4 };

enum class ReplyStatus : quint32 { Ok = 0, BadFormat = 1, TooLarge = 2, DecodeFailed = 3, InputTooLarge = 4 };

constexpr qint64 kMaxBodyBytes = 20 * 1024 * 1024; ///< what a fetch may download
constexpr quint32 kMaxPixels = 16 * 1000 * 1000;   ///< 16 megapixels
constexpr int kAllocationLimitMiB = 64;            ///< QImageReader::setAllocationLimit
constexpr qsizetype kHeaderSize = 20;

/// The format the first bytes say it is (PNG, JPEG, GIF, WebP), None for anything
/// else. Never the Content-Type or the file name.
/// imageFormat (libs/theme/ImageLoad.h) is the wider copy for local files (adds bmp, avif, svg); it is not called
/// from here because the sandboxed helper and the default build must not link each other's code.
ImageFormat sniffFormat(const QByteArray &bytes);

/// The name QImageReader takes for the format, empty for None.
QByteArray formatName(ImageFormat format);

QByteArray encodeHeader(ReplyStatus status, quint32 width, quint32 height, ImageFormat format);

struct DecodedImage {
    QImage image;
    QString error; ///< empty when `image` is set
};

/// The app's check of a helper reply: magic, status, dimensions inside the cap, the
/// format the app sniffed itself, and exactly 4 bytes per pixel after the header.
DecodedImage validateReply(const QByteArray &reply, ImageFormat expected);

} // namespace noteAppC::remoteimage
