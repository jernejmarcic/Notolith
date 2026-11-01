// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "SafeAttachment.h"

#include <QFileInfo>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "ImageLoad.h"
#include "SafeSvg.h"

namespace noteAppC::publish::SafeAttachment {

QString kindOfSuffix(const QString &suffix)
{
    static const QSet<QString> images{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                      QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("avif"),
                                      QStringLiteral("bmp"), QStringLiteral("svg")};
    static const QSet<QString> audio{QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("opus"),
                                     QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("m4a")};
    static const QSet<QString> video{QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv")};
    if (images.contains(suffix))
        return QStringLiteral("image");
    if (audio.contains(suffix))
        return QStringLiteral("audio");
    if (video.contains(suffix))
        return QStringLiteral("video");
    if (suffix == QLatin1String("pdf"))
        return QStringLiteral("pdf");
    return {};
}

namespace {

/// The SVG after safeSvg() (no DTD, comments or outside references) with what a browser would run or draw from
/// elsewhere taken out as well: script, style, foreign content, frames, animation of attributes, every on*
/// attribute and any value that names a javascript: URL.
QByteArray siteSvg(const QByteArray &svg)
{
    static const QSet<QString> dropped{
        QStringLiteral("script"),  QStringLiteral("style"),          QStringLiteral("foreignobject"),
        QStringLiteral("iframe"),  QStringLiteral("object"),         QStringLiteral("embed"),
        QStringLiteral("handler"), QStringLiteral("listener"),       QStringLiteral("set"),
        QStringLiteral("animate"), QStringLiteral("animatemotion"),  QStringLiteral("animatetransform")};
    const QByteArray once = safeSvg(svg);
    if (once.isEmpty())
        return {};
    QXmlStreamReader in(once);
    QByteArray out;
    QXmlStreamWriter writer(&out);
    int skipDepth = 0; // inside a dropped element
    int depth = 0;
    bool root = false;
    while (!in.atEnd()) {
        switch (in.readNext()) {
        case QXmlStreamReader::StartElement: {
            ++depth;
            if (skipDepth > 0 || dropped.contains(in.name().toString().toLower())) {
                ++skipDepth;
                break;
            }
            root = true;
            writer.writeStartElement(in.qualifiedName().toString());
            for (const QXmlStreamNamespaceDeclaration &ns : in.namespaceDeclarations()) {
                if (ns.prefix().isEmpty())
                    writer.writeDefaultNamespace(ns.namespaceUri().toString());
                else
                    writer.writeNamespace(ns.namespaceUri().toString(), ns.prefix().toString());
            }
            for (const QXmlStreamAttribute &a : in.attributes()) {
                const QString name = a.name().toString().toLower();
                const QString value = a.value().toString();
                if (name.startsWith(QLatin1String("on")) || name == QLatin1String("style")
                    || value.contains(QLatin1String("javascript:"), Qt::CaseInsensitive))
                    continue;
                writer.writeAttribute(a.qualifiedName().toString(), value);
            }
            break;
        }
        case QXmlStreamReader::EndElement:
            --depth;
            if (skipDepth > 0)
                --skipDepth;
            else
                writer.writeEndElement();
            break;
        case QXmlStreamReader::Characters:
            if (skipDepth == 0 && depth > 0)
                writer.writeCharacters(in.text().toString());
            break;
        default:
            break;
        }
    }
    if (in.hasError() || !root)
        return {};
    return out;
}

bool startsAt(const QByteArray &b, qsizetype at, const char *magic)
{
    return b.mid(at, qsizetype(qstrlen(magic))) == magic;
}

/// The container a suffix names, told by the file's own first bytes.
bool audioVideoMagic(const QString &suffix, const QByteArray &b)
{
    if (suffix == QLatin1String("mp3"))
        return b.startsWith("ID3") || (b.size() > 1 && quint8(b[0]) == 0xFF && (quint8(b[1]) & 0xE0) == 0xE0);
    if (suffix == QLatin1String("ogg") || suffix == QLatin1String("opus") || suffix == QLatin1String("ogv"))
        return b.startsWith("OggS");
    if (suffix == QLatin1String("flac"))
        return b.startsWith("fLaC");
    if (suffix == QLatin1String("wav"))
        return b.startsWith("RIFF") && startsAt(b, 8, "WAVE");
    if (suffix == QLatin1String("m4a") || suffix == QLatin1String("mp4"))
        return startsAt(b, 4, "ftyp");
    if (suffix == QLatin1String("webm"))
        return b.startsWith("\x1a\x45\xdf\xa3");
    return false;
}

} // namespace

bool prepare(const QString &relativePath, QByteArray &bytes)
{
    const QString suffix = QFileInfo(relativePath).suffix().toLower();
    const QString kind = kindOfSuffix(suffix);
    if (kind == QLatin1String("pdf"))
        return bytes.startsWith("%PDF-");
    if (kind == QLatin1String("audio") || kind == QLatin1String("video"))
        return audioVideoMagic(suffix, bytes);
    if (kind != QLatin1String("image"))
        return false;
    const QByteArray format = noteAppC::imageFormat(bytes);
    if (format.isEmpty() || (format == "svg") != (suffix == QLatin1String("svg")))
        return false;
    if (format == "svg") {
        bytes = siteSvg(bytes);
        return !bytes.isEmpty();
    }
    // jpg and jpeg are both "jpeg"; every other suffix names its own format
    return suffix == QLatin1String(format.constData()) || (format == "jpeg" && suffix == QLatin1String("jpg"));
}

} // namespace noteAppC::publish::SafeAttachment
