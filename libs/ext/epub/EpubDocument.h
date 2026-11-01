// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

#include "EpubArchive.h"

namespace noteAppC {

/// One chapter in reading order.
struct EpubSpineItem {
    QString archivePath; ///< resolved, relative to the archive root, no fragment
    QString manifestID;
};

/// One entry of the table of contents (EPUB2 NCX or EPUB3 nav), nested the
/// way the book nested it.
struct EpubTocEntry {
    QString title;
    QString archivePath; ///< resolved target file, empty if the href was unreadable
    QString fragment;    ///< element id within that file, may be empty
    int spineIndex = -1; ///< -1 when the target is not a spine chapter at all
    QList<EpubTocEntry> children;
};

/// Parses an EPUB 2 or 3 book: the container, the OPF (manifest, spine,
/// metadata) and the table of contents (NCX or nav), and resolves any href
/// found inside the book - a chapter's own path, an image, a stylesheet - to
/// a path inside the archive.
///
/// Everything here reads through EpubArchive; nothing is ever written back
/// and nothing outside the archive, or outside its own scheme-less relative
/// URLs, is ever followed - see resolveArchivePath().
class EpubDocument {
public:
    bool open(const QString &path, QString *error = nullptr);
    bool isValid() const { return m_valid; }
    QString path() const { return m_path; }

    QString title() const { return m_title; }
    QString author() const { return m_author; }
    QString language() const { return m_language; }

    const QList<EpubSpineItem> &spine() const { return m_spine; }
    /// -1 when `archivePath` is not a spine chapter.
    int spineIndexOf(const QString &archivePath) const { return m_spineIndexByPath.value(archivePath, -1); }

    const QList<EpubTocEntry> &tableOfContents() const { return m_toc; }

    /// Raw bytes of any entry in the archive (a chapter, an image, a CSS
    /// file) by its resolved archive path.
    QByteArray readEntry(const QString &archivePath, QString *error = nullptr) const
    {
        return m_archive.read(archivePath, error);
    }

    /// Resolves `href`, as written inside `fromEntry` (e.g. "../images/a.jpg"
    /// inside "text/ch1.xhtml"), to a path inside the archive - the way a
    /// browser resolves a relative URL against its document's own location.
    /// std::nullopt for an empty href, one with a network scheme or a
    /// "data:" URI (never fetched, per the project's HTML rule - see
    /// HtmlRenderer), an archive-absolute href (a leading '/'; EPUB hrefs are
    /// always package-relative, so this is a malformed book, not a case to
    /// support), or one whose "../" segments walk past the archive root -
    /// the one thing a book does not get to do regardless of how it is
    /// nested. A trailing '#fragment' is dropped; callers that need it read
    /// it themselves first (see EpubDocument::open's TOC parsing).
    static std::optional<QString> resolveArchivePath(const QString &fromEntry, const QString &href);

private:
    struct ManifestItem {
        QString archivePath;
        QString mediaType;
        bool isNav = false; // EPUB3 properties="nav"
    };

    void parseContainer(const QByteArray &xml, QString *opfPath, QString *error) const;
    void parseOPF(const QByteArray &xml, const QString &opfPath, QString *error);
    void parseNCX(const QByteArray &xml, const QString &ncxPath);
    void parseNav(const QByteArray &xml, const QString &navPath);
    /// Splits a TOC target's own trailing '#fragment' off before resolving,
    /// then fills in `entry`'s archivePath/fragment/spineIndex - shared by
    /// the NCX and nav parsers, the two places a TOC target is read.
    void resolveTocTarget(EpubTocEntry &entry, const QString &fromEntry, const QString &hrefWithFragment) const;

    EpubArchive m_archive;
    QString m_path;
    bool m_valid = false;

    QString m_title;
    QString m_author;
    QString m_language;

    QHash<QString, ManifestItem> m_manifestByID;
    QList<EpubSpineItem> m_spine;
    QHash<QString, int> m_spineIndexByPath;
    QList<EpubTocEntry> m_toc;
};

} // namespace noteAppC
