// SPDX-License-Identifier: GPL-3.0-or-later
#include "SafeAttachment.h"

#include <QFileInfo>
#include <QRegularExpression>
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

const QString kSvgNS = QStringLiteral("http://www.w3.org/2000/svg");
const QString kXlinkNS = QStringLiteral("http://www.w3.org/1999/xlink");

/// The SVG elements a site keeps. Anything else, and anything in another namespace, goes with its subtree.
bool keptElement(const QString &name)
{
    static const QSet<QString> kept{
        QStringLiteral("svg"), QStringLiteral("g"), QStringLiteral("defs"), QStringLiteral("symbol"),
        QStringLiteral("use"), QStringLiteral("title"), QStringLiteral("desc"), QStringLiteral("path"),
        QStringLiteral("rect"), QStringLiteral("circle"), QStringLiteral("ellipse"), QStringLiteral("line"),
        QStringLiteral("polyline"), QStringLiteral("polygon"), QStringLiteral("text"), QStringLiteral("tspan"),
        QStringLiteral("textPath"), QStringLiteral("linearGradient"), QStringLiteral("radialGradient"),
        QStringLiteral("stop"), QStringLiteral("pattern"), QStringLiteral("clipPath"), QStringLiteral("mask"),
        QStringLiteral("marker"), QStringLiteral("image"), QStringLiteral("filter"), QStringLiteral("feBlend"),
        QStringLiteral("feColorMatrix"), QStringLiteral("feComponentTransfer"), QStringLiteral("feComposite"),
        QStringLiteral("feConvolveMatrix"), QStringLiteral("feDiffuseLighting"), QStringLiteral("feDisplacementMap"),
        QStringLiteral("feDistantLight"), QStringLiteral("feDropShadow"), QStringLiteral("feFlood"),
        QStringLiteral("feFuncA"), QStringLiteral("feFuncB"), QStringLiteral("feFuncG"), QStringLiteral("feFuncR"),
        QStringLiteral("feGaussianBlur"), QStringLiteral("feImage"), QStringLiteral("feMerge"),
        QStringLiteral("feMergeNode"), QStringLiteral("feMorphology"), QStringLiteral("feOffset"),
        QStringLiteral("fePointLight"), QStringLiteral("feSpecularLighting"), QStringLiteral("feSpotLight"),
        QStringLiteral("feTile"), QStringLiteral("feTurbulence")};
    return kept.contains(name);
}

/// A reference that stays in the document: after taking out every space and control character, "#fragment".
/// `raster` also lets an inline PNG, JPEG, GIF or WebP through (an <image>).
bool localReference(const QString &value, bool raster)
{
    QString v;
    for (const QChar c : value) {
        if (c.unicode() > 0x20 && c.unicode() != 0x7f)
            v += c;
    }
    if (v.startsWith(QLatin1Char('#')))
        return true;
    static const QRegularExpression inlineRaster(QStringLiteral("^data:image/(png|jpeg|gif|webp)[;,]"),
                                                 QRegularExpression::CaseInsensitiveOption);
    return raster && inlineRaster.match(v).hasMatch();
}

/// Whether every url(...) in `value` points at "#fragment", and nothing is written in a way that hides one.
bool onlyLocalURLs(const QString &value)
{
    if (value.contains(QLatin1Char('\\')))
        return false; // a CSS escape can spell url( without writing it
    qsizetype at = 0;
    while ((at = value.indexOf(QLatin1String("url("), at, Qt::CaseInsensitive)) >= 0) {
        const qsizetype close = value.indexOf(QLatin1Char(')'), at);
        if (close < 0)
            return false;
        QString target = value.mid(at + 4, close - at - 4).trimmed();
        if (target.size() > 1 && (target.startsWith(QLatin1Char('"')) || target.startsWith(QLatin1Char('\''))))
            target = target.mid(1).trimmed();
        if (!target.startsWith(QLatin1Char('#')))
            return false;
        at = close;
    }
    return true;
}

/// The SVG, rebuilt: after safeSvg() (no DTD, comments or outside references), only the allowed SVG elements,
/// each with only the attributes that run nothing and fetch nothing. The root must be an SVG `svg`; otherwise
/// the result is empty.
QByteArray siteSvg(const QByteArray &svg)
{
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
            const QString name = in.name().toString();
            const bool isSvg = in.namespaceUri() == kSvgNS;
            if (depth == 0 && !(isSvg && name == QLatin1String("svg")))
                return {};
            ++depth;
            if (skipDepth > 0 || !isSvg || !keptElement(name)) {
                ++skipDepth;
                break;
            }
            writer.writeStartElement(name);
            if (!root) {
                root = true;
                writer.writeDefaultNamespace(kSvgNS);
                writer.writeNamespace(kXlinkNS, QStringLiteral("xlink"));
            }
            const bool raster = name == QLatin1String("image") || name == QLatin1String("feImage");
            for (const QXmlStreamAttribute &a : in.attributes()) {
                const QString ns = a.namespaceUri().toString();
                const QString attr = a.name().toString();
                const QString value = a.value().toString();
                if (!ns.isEmpty() && ns != kSvgNS && ns != kXlinkNS)
                    continue;
                // style can fetch through image-set() and other CSS that has no url(): never kept.
                if (attr.startsWith(QLatin1String("on"), Qt::CaseInsensitive)
                    || attr.compare(QLatin1String("style"), Qt::CaseInsensitive) == 0)
                    continue;
                if (attr.compare(QLatin1String("href"), Qt::CaseInsensitive) == 0 && !localReference(value, raster))
                    continue;
                if (value.contains(QLatin1String("javascript:"), Qt::CaseInsensitive) || !onlyLocalURLs(value)
                    || value.contains(QLatin1String("@import"), Qt::CaseInsensitive))
                    continue;
                writer.writeAttribute(ns == kXlinkNS ? QStringLiteral("xlink:") + attr : attr, value);
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
