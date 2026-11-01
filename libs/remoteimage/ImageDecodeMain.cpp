// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// notolith-imagedecode: decodes one remote image so the app never does.
//
// Run by the app under bubblewrap (no network, no home, the syscall filter, rlimits). Reads the image's bytes
// on stdin until EOF and writes one reply on stdout (ImageWire.h). With --serve it instead reads frames of a
// 4-byte little-endian length and that many bytes, and answers each with one reply.
//
// The format is picked from the magic bytes and the reader is told which one: no autodetection, so a PNG
// header cannot be fed to another decoder. The pixel count is read from the header and checked before the
// decode, and QImageReader's own allocation limit applies on top.

#include "ImageWire.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>

#include <cstdio>
#include <cstring>

using namespace noteAppC::remoteimage;

namespace {

QByteArray failure(ReplyStatus status)
{
    return encodeHeader(status, 0, 0, ImageFormat::None);
}

QByteArray decode(const QByteArray &bytes)
{
    if (bytes.size() > kMaxBodyBytes)
        return failure(ReplyStatus::InputTooLarge);
    const ImageFormat format = sniffFormat(bytes);
    if (format == ImageFormat::None)
        return failure(ReplyStatus::BadFormat);

    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, formatName(format));
    reader.setAutoDetectImageFormat(false);
    reader.setAllocationLimit(kAllocationLimitMiB);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0)
        return failure(ReplyStatus::DecodeFailed);
    if (quint64(size.width()) * quint64(size.height()) > kMaxPixels)
        return failure(ReplyStatus::TooLarge);
    QImage image = reader.read(); // a GIF gives its first frame
    if (image.isNull())
        return failure(ReplyStatus::DecodeFailed);
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull() || quint64(image.width()) * quint64(image.height()) > kMaxPixels)
        return failure(ReplyStatus::TooLarge);
    QByteArray reply = encodeHeader(ReplyStatus::Ok, quint32(image.width()), quint32(image.height()), format);
    reply.append(reinterpret_cast<const char *>(image.constBits()), qsizetype(image.width()) * image.height() * 4);
    return reply;
}

// Reads up to `limit` bytes plus one, so an input that is too long is known to be.
QByteArray readAll(std::FILE *in, qint64 limit)
{
    QByteArray data;
    char chunk[65536];
    while (data.size() <= limit) {
        const size_t got = std::fread(chunk, 1, sizeof chunk, in);
        if (got == 0)
            break;
        data.append(chunk, qsizetype(got));
    }
    return data;
}

bool writeAll(const QByteArray &reply)
{
    return std::fwrite(reply.constData(), 1, size_t(reply.size()), stdout) == size_t(reply.size()) && std::fflush(stdout) == 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--serve") == 0) {
        for (;;) {
            unsigned char head[4];
            if (std::fread(head, 1, 4, stdin) != 4)
                return 0;
            const quint32 length = quint32(head[0]) | quint32(head[1]) << 8 | quint32(head[2]) << 16 | quint32(head[3]) << 24;
            if (length > quint32(kMaxBodyBytes))
                return 2; // the app never sends this; end rather than skip a body of unknown size
            QByteArray body(qsizetype(length), Qt::Uninitialized);
            if (length && std::fread(body.data(), 1, length, stdin) != length)
                return 2;
            if (!writeAll(decode(body)))
                return 1;
        }
    }
    return writeAll(decode(readAll(stdin, kMaxBodyBytes))) ? 0 : 1;
}
