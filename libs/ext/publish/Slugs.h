// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>
#include <QStringList>

namespace noteAppC::publish {

/// URL and anchor rules for the static export, gathered in one small header
/// so Publisher.cpp and PageBody.cpp read the same rule rather than two.
///
/// Obsidian Publish's own docs (obsidian.md/help/publish/permalinks) say only
/// that folders become URL segments and that a `permalink` property renames
/// the whole URL; they do not spell out character handling. The rule below is
/// this project's own, chosen for what a static site needs: stable (a pure
/// function of the vault path, independent of publish order), collision-free
/// (a dedup pass, not a hope), and safe in a URL and a filesystem path alike.
struct Slugs {
    /// One path segment (a folder name or a filename minus extension) to a
    /// URL-safe, lower-case slug: run every character outside `[a-z0-9]`
    /// together into a single '-', trim leading/trailing '-'. An empty result
    /// (a segment that was all punctuation) becomes "note" rather than "" -
    /// an empty path segment would collapse two directory levels into one.
    static QString slugifySegment(const QString &text);

    /// The default output path for `vaultRelativePath` (folders kept, each
    /// segment slugified on its own, ".md" dropped, ".html"
    /// added). Never consults `permalink` - see permalinkOverride() for that.
    static QString defaultPagePath(const QString &vaultRelativePath);

    /// `permalink` property value turned into an output path, or empty when
    /// `raw` is empty or tries to leave the site root (a leading "..", or a
    /// segment that is exactly ".."), in which case the caller falls back to
    /// defaultPagePath(). Obsidian's own semantics: the property replaces the
    /// whole URL, not just the last segment.
    static QString permalinkOverride(const QString &raw);

    /// Heading text to an anchor id: lower-case, run non `[a-z0-9]` together
    /// into '-', trim; "heading" when that leaves nothing (a heading that was
    /// only punctuation or emoji).
    static QString headingAnchor(const QString &headingText);

    /// A block id (already `[A-Za-z0-9][A-Za-z0-9-]*` by ObsidianSyntax's own
    /// grammar) to the element id a block-reference link jumps to. Prefixed
    /// so a block id can never collide with a heading anchor that happens to
    /// read the same, and so the id is never itself a bare number (invalid
    /// before HTML5, and still worth avoiding for a tag name mismatch).
    static QString blockAnchor(const QString &blockId);

    /// Where a tag's generated page lives, site-root-relative. A nested tag
    /// ("status/active") keeps its own folder under tags/, the same rule
    /// defaultPagePath() applies to a note's own folders, so "status/active"
    /// and "status/review" do not collide with each other or with a literal
    /// tag called "active".
    static QString tagPagePath(const QString &tagName);

    /// Makes every path in `paths` unique in place, appending "-2", "-3", ...
    /// to the final path segment (before ".html") of each path that repeats
    /// one seen earlier in the list - so the first occurrence, in whatever
    /// order the caller already sorted `paths`, keeps its plain slug. Case
    /// preserved, comparison case-sensitive: this runs against a real
    /// filesystem's output directory.
    static void deduplicate(QStringList &paths);

    /// The relative href from a page at `fromOutputPath` to a target at
    /// `toOutputPath`, both site-root-relative paths as defaultPagePath()/
    /// permalinkOverride() produce them - so every page works from `file://`
    /// and from any sub-path the site is served under, per the task: no
    /// absolute (site-root-rooted) hrefs anywhere.
    static QString relativeHref(const QString &fromOutputPath, const QString &toOutputPath);
};

} // namespace noteAppC::publish
