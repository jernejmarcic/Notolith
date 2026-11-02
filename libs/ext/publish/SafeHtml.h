// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace noteAppC::publish::SafeHtml {

/// Whether a note's link or media target may be put on a published page: an
/// http, https or mailto URL, or a relative one. Every other scheme
/// (javascript:, vbscript:, data:, file:, anything unknown) is refused. The
/// check reads the URL the way a browser does: leading control characters and
/// spaces are dropped and tabs and line breaks inside it are ignored.
bool isSafeURL(const QString &url);

/// Raw HTML from a note, made safe for a published page. Kept: a fixed set of
/// ordinary elements and attributes, links and media only with a safe URL, a
/// style attribute only without url() and the like. Dropped, content included:
/// script, style, iframe, object, embed, form, template, svg and math. Dropped,
/// content kept: any other element. No on* attribute and no srcdoc survives.
/// `singleTag` is for CommonMark's inline raw HTML, which arrives one tag at a
/// time: the tag alone is returned, not closed.
/// Without gumbo the HTML is returned as escaped text.
QString sanitize(const QString &raw, bool singleTag);

} // namespace noteAppC::publish::SafeHtml
