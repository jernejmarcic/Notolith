// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include "SourceMap.h"

namespace noteAppC {

struct Heading {
    int level = 1;
    QString text;    ///< plain text, delimiters and inline markup removed
    Range range;     ///< the whole heading line
    Range textRange; ///< just the text, for renaming
};

/// One link out of a note: a wikilink, an embed, or a CommonMark link.
struct Link {
    QString raw;      ///< source between the brackets, unresolved
    QString target;   ///< note or file name, "" for a link inside this note
    QString subpath;  ///< "#heading", "#^block-id", or "" (leading # kept)
    QString alias;    ///< display text after "|" or in "[text](...)"
    bool embed = false;
    bool wiki = true; ///< false for [text](target)
    bool external = false;
    Range range;
    Range targetRange; ///< the target alone, so a rename rewrites only it
};

struct Tag {
    QString name; ///< without the "#", nesting kept as "a/b"
    Range range;
};

struct BlockId {
    QString id;  ///< without the "^"
    Range range; ///< the "^id" marker
};

struct Task {
    QChar state = QLatin1Char(' '); ///< what is in the box; see ListItemSpan::state
    QString text;
    Range range;
};

/// A frontmatter key. `values` holds one entry for a scalar, many for a list.
///
/// `raw` is the same list as it is spelled in the file - quotes, dates and all
/// - so the properties editor can rewrite one value without reflowing the YAML
/// around it. `range` covers the whole entry, key line included.
struct Property {
    /// Obsidian's six property types. `Unknown` is not one of them; it is what
    /// a lookup that found nothing returns.
    enum class Type { Text, List, Number, Checkbox, Date, DateTime, Unknown };

    QString key;
    Type type = Type::Text;
    QStringList values;
    QStringList raw;
    Range range;
};

/// Everything the index keeps about one note. Produced by Parser, stored by
/// libs/index, and the only thing most views ever read.
struct Metadata {
    QString path;    ///< absolute
    qint64 mtime = 0;
    qint64 size = 0;
    QByteArray hash; ///< of the file contents, to catch mtime-only touches

    Range frontmatter;      ///< the whole block including both --- lines
    QList<Property> properties;
    QStringList aliases;    ///< from the `aliases` property
    QStringList cssClasses; ///< from `cssclasses`
    QString id;             ///< from `id`, an implicit alias (Ideas.md 16.5)

    QList<Heading> headings;
    QList<Link> links;
    QList<Tag> tags; ///< inline tags and frontmatter tags, both
    QList<BlockId> blockIds;
    QList<Task> tasks;
    /// Meta-note runs, delimiters included, in source order: filled only when
    /// the parse was asked for them (ParseOptions::metaNotes).
    QList<Range> metaNotes;

    int wordCount = 0;
    QStringList parseWarnings;

    /// Every name this note answers to in a wikilink.
    QStringList linkNames() const;
};

} // namespace noteAppC
