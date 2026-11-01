// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "EpubDocument.h"

#include <QStringList>
#include <QUrl>
#include <QXmlStreamReader>

#include <functional>

namespace noteAppC {

std::optional<QString> EpubDocument::resolveArchivePath(const QString &fromEntry, const QString &hrefIn)
{
    return resolveEpubPath(fromEntry, hrefIn);
}

void EpubDocument::resolveTocTarget(EpubTocEntry &entry, const QString &fromEntry,
                                    const QString &hrefWithFragment) const
{
    const qsizetype hash = hrefWithFragment.indexOf(QLatin1Char('#'));
    const QString hrefPart = hash < 0 ? hrefWithFragment : hrefWithFragment.left(hash);
    entry.fragment = hash < 0 ? QString() : hrefWithFragment.mid(hash + 1);
    if (const auto resolved = resolveArchivePath(fromEntry, hrefPart)) {
        entry.archivePath = *resolved;
        entry.spineIndex = m_spineIndexByPath.value(*resolved, -1);
    }
}

void EpubDocument::parseContainer(const QByteArray &xml, QString *opfPath, QString *error) const
{
    QXmlStreamReader reader(xml);
    while (reader.readNextStartElement()) {
        if (reader.name() != QLatin1String("container")) {
            reader.skipCurrentElement();
            continue;
        }
        while (reader.readNextStartElement()) {
            if (reader.name() != QLatin1String("rootfiles")) {
                reader.skipCurrentElement();
                continue;
            }
            while (reader.readNextStartElement()) {
                if (reader.name() == QLatin1String("rootfile")) {
                    const QString mediaType = reader.attributes().value(QLatin1String("media-type")).toString();
                    const QString fullPath = reader.attributes().value(QLatin1String("full-path")).toString();
                    if (opfPath->isEmpty()
                        && (mediaType == QLatin1String("application/oebps-package+xml")
                           || fullPath.endsWith(QLatin1String(".opf"), Qt::CaseInsensitive)))
                        *opfPath = fullPath;
                    reader.skipCurrentElement();
                } else {
                    reader.skipCurrentElement();
                }
            }
        }
    }
    if (reader.hasError() && error)
        *error = reader.errorString();
}

void EpubDocument::parseOPF(const QByteArray &xml, const QString &opfPath, QString *error)
{
    QXmlStreamReader reader(xml);
    QString spineTocID;
    bool inMetadata = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isEndElement() && reader.name() == QLatin1String("metadata")) {
            inMetadata = false;
        } else if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name == QLatin1String("metadata")) {
                inMetadata = true;
            } else if (inMetadata && name == QLatin1String("title") && m_title.isEmpty()) {
                m_title = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            } else if (inMetadata && name == QLatin1String("creator") && m_author.isEmpty()) {
                m_author = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            } else if (inMetadata && name == QLatin1String("language") && m_language.isEmpty()) {
                m_language = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            } else if (name == QLatin1String("item")) {
                const QString id = reader.attributes().value(QLatin1String("id")).toString();
                const QString href = reader.attributes().value(QLatin1String("href")).toString();
                const QString mediaType = reader.attributes().value(QLatin1String("media-type")).toString();
                const QString properties = reader.attributes().value(QLatin1String("properties")).toString();
                if (!id.isEmpty() && !href.isEmpty()) {
                    if (const auto resolved = resolveArchivePath(opfPath, href)) {
                        ManifestItem item;
                        item.archivePath = *resolved;
                        item.mediaType = mediaType;
                        item.isNav = properties.split(QLatin1Char(' '), Qt::SkipEmptyParts)
                                        .contains(QLatin1String("nav"));
                        m_manifestByID.insert(id, item);
                    }
                }
            } else if (name == QLatin1String("spine")) {
                spineTocID = reader.attributes().value(QLatin1String("toc")).toString();
            } else if (name == QLatin1String("itemref")) {
                const QString idref = reader.attributes().value(QLatin1String("idref")).toString();
                const auto it = m_manifestByID.constFind(idref);
                if (it != m_manifestByID.constEnd()) {
                    m_spineIndexByPath.insert(it->archivePath, int(m_spine.size()));
                    m_spine.append(EpubSpineItem{it->archivePath, idref});
                }
            }
        }
    }
    if (reader.hasError() && m_spine.isEmpty() && error)
        *error = reader.errorString();

    // Table of contents: an EPUB3 nav document wins if the manifest declares
    // one (properties="nav"); otherwise fall back to the EPUB2 spine@toc
    // pointing at an NCX by manifest id. A book with neither just has no TOC
    // - the reader falls back to spine order (see EpubView).
    QString navPath;
    for (auto it = m_manifestByID.constBegin(); it != m_manifestByID.constEnd(); ++it) {
        if (it->isNav) {
            navPath = it->archivePath;
            break;
        }
    }
    if (!navPath.isEmpty()) {
        const QByteArray navXML = m_archive.read(navPath);
        if (!navXML.isEmpty())
            parseNav(navXML, navPath);
    } else if (!spineTocID.isEmpty()) {
        const auto it = m_manifestByID.constFind(spineTocID);
        if (it != m_manifestByID.constEnd()) {
            const QByteArray ncxXML = m_archive.read(it->archivePath);
            if (!ncxXML.isEmpty())
                parseNCX(ncxXML, it->archivePath);
        }
    }
}

void EpubDocument::parseNCX(const QByteArray &xml, const QString &ncxPath)
{
    QXmlStreamReader reader(xml);

    std::function<EpubTocEntry()> readNavPoint; // reader at <navPoint>, consumes to </navPoint>
    readNavPoint = [&]() -> EpubTocEntry {
        EpubTocEntry entry;
        while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("navLabel")) {
                while (reader.readNextStartElement()) {
                    if (reader.name() == QLatin1String("text"))
                        entry.title = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                    else
                        reader.skipCurrentElement();
                }
            } else if (reader.name() == QLatin1String("content")) {
                resolveTocTarget(entry, ncxPath, reader.attributes().value(QLatin1String("src")).toString());
                reader.skipCurrentElement();
            } else if (reader.name() == QLatin1String("navPoint")) {
                entry.children.append(readNavPoint());
            } else {
                reader.skipCurrentElement();
            }
        }
        return entry;
    };

    while (reader.readNextStartElement()) {
        if (reader.name() != QLatin1String("ncx")) {
            reader.skipCurrentElement();
            continue;
        }
        while (reader.readNextStartElement()) {
            if (reader.name() != QLatin1String("navMap")) {
                reader.skipCurrentElement();
                continue;
            }
            while (reader.readNextStartElement()) {
                if (reader.name() == QLatin1String("navPoint"))
                    m_toc.append(readNavPoint());
                else
                    reader.skipCurrentElement();
            }
        }
    }
}

void EpubDocument::parseNav(const QByteArray &xml, const QString &navPath)
{
    QXmlStreamReader reader(xml);

    std::function<QList<EpubTocEntry>()> readList; // reader at <ol>, consumes to </ol>
    std::function<EpubTocEntry()> readItem;         // reader at <li>, consumes to </li>

    readItem = [&]() -> EpubTocEntry {
        EpubTocEntry entry;
        while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("a")) {
                const QString href = reader.attributes().value(QLatin1String("href")).toString();
                entry.title = reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                if (!href.isEmpty())
                    resolveTocTarget(entry, navPath, href);
            } else if (reader.name() == QLatin1String("ol")) {
                entry.children = readList();
            } else {
                reader.skipCurrentElement();
            }
        }
        return entry;
    };
    readList = [&]() -> QList<EpubTocEntry> {
        QList<EpubTocEntry> items;
        while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("li"))
                items.append(readItem());
            else
                reader.skipCurrentElement();
        }
        return items;
    };

    // The nav document can hold several <nav> sections (toc, landmarks,
    // page-list); only "epub:type" containing "toc" is the table of
    // contents. Matched by local attribute name, not namespace URI - the
    // prefix bound to it is always "epub" in practice and this is simpler.
    while (!reader.atEnd() && m_toc.isEmpty()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("nav"))
            continue;
        bool isToc = false;
        for (const auto &attr : reader.attributes()) {
            if (attr.name() == QLatin1String("type") && attr.value().contains(QLatin1String("toc"))) {
                isToc = true;
                break;
            }
        }
        if (!isToc)
            continue;
        while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("ol"))
                m_toc = readList();
            else
                reader.skipCurrentElement();
        }
    }
}

bool EpubDocument::open(const QString &path, QString *error)
{
    m_valid = false;
    m_path.clear();
    m_title.clear();
    m_author.clear();
    m_language.clear();
    m_manifestByID.clear();
    m_spine.clear();
    m_spineIndexByPath.clear();
    m_toc.clear();

    if (!m_archive.open(path, error))
        return false;

    QString containerError;
    const QByteArray containerXML = m_archive.read(QStringLiteral("META-INF/container.xml"), &containerError);
    if (containerXML.isEmpty()) {
        if (error)
            *error = containerError.isEmpty() ? QStringLiteral("no META-INF/container.xml") : containerError;
        return false;
    }

    QString opfPath;
    QString containerParseError;
    parseContainer(containerXML, &opfPath, &containerParseError);
    if (opfPath.isEmpty()) {
        if (error)
            *error = containerParseError.isEmpty() ? QStringLiteral("container.xml names no rootfile")
                                                    : containerParseError;
        return false;
    }

    QString opfError;
    const QByteArray opfXML = m_archive.read(opfPath, &opfError);
    if (opfXML.isEmpty()) {
        if (error)
            *error = opfError;
        return false;
    }

    QString opfParseError;
    parseOPF(opfXML, opfPath, &opfParseError);
    if (m_spine.isEmpty()) {
        if (error)
            *error = opfParseError.isEmpty() ? QStringLiteral("no readable spine in %1").arg(opfPath)
                                             : opfParseError;
        return false;
    }

    m_path = path;
    m_valid = true;
    return true;
}

} // namespace noteAppC
