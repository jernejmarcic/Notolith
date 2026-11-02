// SPDX-License-Identifier: GPL-3.0-or-later
#include "SafeSvg.h"

#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace noteAppC {

namespace {

/// A picture format Qt decodes without looking at anything else: told by its first bytes, so the
/// common case does not load every image plugin to ask.
bool isRasterMagic(const QByteArray &b)
{
    return b.startsWith("\x89PNG") || b.startsWith("\xff\xd8\xff") || b.startsWith("GIF8") || b.startsWith("BM")
        || (b.size() > 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP");
}

/// `data:image/<png|jpeg|gif|webp>;base64,<payload>` whose payload really starts like such a picture:
/// Qt decodes the bytes by content, not by the type the URI claims, so an "image/png" holding an SVG
/// would be an SVG with references of its own.
bool isPlainPictureURI(const QString &value)
{
    static const char *const types[] = {"png", "jpeg", "gif", "webp"};
    const QString v = value.trimmed();
    for (const char *type : types) {
        const QString head = QStringLiteral("data:image/%1;base64,").arg(QLatin1String(type));
        if (v.startsWith(head, Qt::CaseInsensitive)) {
            const QByteArray payload = v.mid(head.size(), 32).toLatin1(); // 24 characters are 18 bytes
            return isRasterMagic(QByteArray::fromBase64(payload.left(24)));
        }
    }
    return false;
}

} // namespace

QByteArray safeSvg(const QByteArray &svg)
{
    QXmlStreamReader in(svg);
    QByteArray out;
    out.reserve(svg.size());
    QXmlStreamWriter writer(&out);
    bool root = false;
    int depth = 0;
    while (!in.atEnd()) {
        switch (in.readNext()) {
        case QXmlStreamReader::StartElement: {
            root = true;
            ++depth;
            writer.writeStartElement(in.qualifiedName().toString());
            for (const QXmlStreamNamespaceDeclaration &ns : in.namespaceDeclarations()) {
                if (ns.prefix().isEmpty())
                    writer.writeDefaultNamespace(ns.namespaceUri().toString());
                else
                    writer.writeNamespace(ns.namespaceUri().toString(), ns.prefix().toString());
            }
            for (const QXmlStreamAttribute &a : in.attributes()) {
                const QString value = a.value().toString();
                if (a.name() == QLatin1String("href") && !value.startsWith(QLatin1Char('#')) && !isPlainPictureURI(value))
                    continue;
                if (a.qualifiedName() == QLatin1String("xml:base"))
                    continue;
                writer.writeAttribute(a.qualifiedName().toString(), value);
            }
            break;
        }
        case QXmlStreamReader::EndElement:
            --depth;
            writer.writeEndElement();
            break;
        case QXmlStreamReader::Characters:
            if (depth > 0)
                writer.writeCharacters(in.text().toString());
            break;
        default: // the DTD, comments, processing instructions and the entity references left over
            break;
        }
    }
    if (in.hasError() || !root || depth != 0)
        return {};
    return out;
}

} // namespace noteAppC
