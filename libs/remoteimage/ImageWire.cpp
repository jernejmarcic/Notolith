// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageWire.h"

#include <QCoreApplication>

#include <cstring>

namespace noteAppC::remoteimage {

namespace {

void put32(QByteArray &out, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.append(char((value >> shift) & 0xff));
}

quint32 get32(const char *at)
{
    quint32 value = 0;
    for (int n = 0; n < 4; ++n)
        value |= quint32(uchar(at[n])) << (8 * n);
    return value;
}

QString statusMessage(quint32 status)
{
    switch (ReplyStatus(status)) {
    case ReplyStatus::BadFormat:
        return QCoreApplication::translate("RemoteImage", "Not a PNG, JPEG, GIF or WebP image.");
    case ReplyStatus::TooLarge:
        return QCoreApplication::translate("RemoteImage", "The image is over 16 megapixels.");
    case ReplyStatus::InputTooLarge:
        return QCoreApplication::translate("RemoteImage", "The image file is over 20 MiB.");
    default:
        return QCoreApplication::translate("RemoteImage", "The image could not be decoded.");
    }
}

} // namespace

ImageFormat sniffFormat(const QByteArray &bytes)
{
    if (bytes.startsWith("\x89PNG\r\n\x1a\n"))
        return ImageFormat::PNG;
    if (bytes.startsWith("\xff\xd8\xff"))
        return ImageFormat::JPEG;
    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
        return ImageFormat::GIF;
    if (bytes.size() >= 12 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP")
        return ImageFormat::WebP;
    return ImageFormat::None;
}

QByteArray formatName(ImageFormat format)
{
    switch (format) {
    case ImageFormat::PNG:
        return "png";
    case ImageFormat::JPEG:
        return "jpeg";
    case ImageFormat::GIF:
        return "gif";
    case ImageFormat::WebP:
        return "webp";
    case ImageFormat::None:
        break;
    }
    return {};
}

QByteArray encodeHeader(ReplyStatus status, quint32 width, quint32 height, ImageFormat format)
{
    QByteArray out("NIMG", 4);
    put32(out, quint32(status));
    put32(out, width);
    put32(out, height);
    put32(out, quint32(format));
    return out;
}

DecodedImage validateReply(const QByteArray &reply, ImageFormat expected)
{
    const auto fail = [](const QString &why) { return DecodedImage{QImage(), why}; };
    if (reply.size() < kHeaderSize || std::memcmp(reply.constData(), "NIMG", 4) != 0)
        return fail(QCoreApplication::translate("RemoteImage", "The image decoder gave no usable answer."));
    const quint32 status = get32(reply.constData() + 4);
    if (status != quint32(ReplyStatus::Ok))
        return fail(statusMessage(status));
    const quint32 width = get32(reply.constData() + 8);
    const quint32 height = get32(reply.constData() + 12);
    const quint32 format = get32(reply.constData() + 16);
    const quint64 pixels = quint64(width) * height;
    if (width == 0 || height == 0 || pixels > kMaxPixels || format != quint32(expected)
        || quint64(reply.size()) != quint64(kHeaderSize) + pixels * 4)
        return fail(QCoreApplication::translate("RemoteImage", "The image decoder's answer did not match."));
    const QImage view(reinterpret_cast<const uchar *>(reply.constData()) + kHeaderSize, int(width), int(height),
                      int(width) * 4, QImage::Format_ARGB32_Premultiplied);
    return {view.copy(), {}};
}

} // namespace noteAppC::remoteimage
