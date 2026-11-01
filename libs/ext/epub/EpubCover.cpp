#include "EpubCover.h"
#include "ImageLoad.h"

#include "EpubArchive.h"

#include <QHash>
#include <QStringList>
#include <QXmlStreamReader>

namespace noteAppC {

namespace {

/// The OPF's path from META-INF/container.xml; empty when there is none.
QString opfPathOf(const EpubArchive &archive)
{
    QXmlStreamReader reader(archive.read(QStringLiteral("META-INF/container.xml")));
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("rootfile"))
            return reader.attributes().value(QLatin1String("full-path")).toString();
    }
    return {};
}

/// The archive entry of the cover image, from the OPF's manifest and metadata.
QString coverEntryOf(const QByteArray &opf, const QString &opfPath)
{
    QHash<QString, QString> hrefByID; // manifest id -> href
    QString byProperty, metaID;
    QXmlStreamReader reader(opf);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const QXmlStreamAttributes attributes = reader.attributes();
        if (reader.name() == QLatin1String("item")) {
            const QString href = attributes.value(QLatin1String("href")).toString();
            hrefByID.insert(attributes.value(QLatin1String("id")).toString(), href);
            if (byProperty.isEmpty()
                && attributes.value(QLatin1String("properties")).toString()
                       .split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QLatin1String("cover-image")))
                byProperty = href;
        } else if (reader.name() == QLatin1String("meta")
                   && attributes.value(QLatin1String("name")) == QLatin1String("cover")) {
            metaID = attributes.value(QLatin1String("content")).toString();
        }
    }
    const QString href = byProperty.isEmpty() ? hrefByID.value(metaID) : byProperty;
    return resolveEpubPath(opfPath, href).value_or(QString());
}

} // namespace

QImage epubCover(const QString &path, int maxSide)
{
    EpubArchive archive;
    if (!archive.open(path))
        return {};
    const QString opfPath = opfPathOf(archive);
    if (opfPath.isEmpty() || !archive.contains(opfPath))
        return {};
    const QString entry = coverEntryOf(archive.read(opfPath), opfPath);
    if (entry.isEmpty() || !archive.contains(entry))
        return {};
    QImage cover = decodeImage(archive.read(entry)); // a book's SVG cover may not pull in files either
    if (cover.isNull() || (cover.width() <= maxSide && cover.height() <= maxSide))
        return cover;
    return cover.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace noteAppC
