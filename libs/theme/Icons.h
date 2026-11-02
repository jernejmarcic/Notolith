// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QString>

namespace noteAppC {

#ifdef NOTEAPPC_HAVE_NETWORK
/// Fires once a remote icon has finished downloading. One instance for the
/// whole process, reached through Icons::notifier(), so a widget can connect
/// once and repaint whenever any icon it asked for arrives, rather than every
/// caller of Icons::icon() owning its own QNetworkAccessManager.
class IconsNotifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

signals:
    /// `name` is the exact string passed to Icons::icon() - only a remote
    /// https URL ever fires this, and only once it is loaded, never on
    /// failure. Call Icons::icon() again afterwards to pick up the result.
    void iconReady(const QString &name);
};
#endif

/// Lucide icons (ISC), the set Obsidian itself draws with, recoloured to a
/// theme token at load - and, in the same slot, a callout's own icon: a local
/// image file, a remote URL, or a Nerd Font glyph.
class Icons {
public:
    /// `name` takes one of four forms:
    ///
    /// 1. A Lucide id ("folder-open"), resolved from the bundled `:/icons`
    ///    resource.
    /// 2. An absolute path or a `file://` URL to a local .svg/.png/.jpg/.webp.
    /// 3. An `https://` URL to a PNG, JPEG, GIF or WebP, in a build with NOTEAPPC_WITH_NETWORK only: the default
    ///    build never fetches, and a URL gives a null icon. It is fetched and decoded like a remote image in a
    ///    note (libs/remoteimage, docs/OFFLINE.md): http, local addresses and SVG are refused.
    /// 4. A Nerd Font glyph, written either as a hex codepoint (`U+E73C`,
    ///    case-insensitive, 4-6 hex digits) or as the literal glyph character
    ///    itself - a string that is exactly one Unicode code point outside
    ///    ASCII (checked with QString::toUcs4()), typed or pasted straight
    ///    from a cheatsheet. Drawn with the first installed font family whose
    ///    name contains "Nerd Font" (QFontDatabase::families(), cached once
    ///    per process); with none installed, or the chosen font missing that
    ///    exact glyph, this returns a null icon rather than a tofu box or a
    ///    look-alike from a fallback font - a wrong icon should never appear
    ///    to be the right one.
    ///
    /// Every result is scaled to `size` and centred inside the square,
    /// preserving its own aspect ratio (a glyph by its own ink, everything
    /// else by its natural size), so a photograph or a Nerd Font glyph lines
    /// up with a Lucide icon in the same column rather than stretching to
    /// fill it.
    ///
    /// `color` recolours an SVG that spells `currentColor` - the Lucide set
    /// does, so today's callers are unaffected - and a Nerd Font glyph, which
    /// is always tinted like one. A user's own image, SVG or raster, is drawn
    /// as-is and never tinted.
    ///
    /// Returns a null icon when the resource is missing, which shows up as a
    /// blank button rather than a crash. This is also what a remote URL
    /// returns before it is cached: this call never blocks on the network.
    /// The fetch starts in the background, and notifier()'s iconReady(name)
    /// fires once the picture is there, so the caller can repaint and call
    /// icon() again to pick it up. A fetch that fails is remembered so it is
    /// not retried on every paint.
    static QIcon icon(const QString &name, const QColor &color, int size = 18);

    /// Whether a remote URL may be fetched or read from the cache at all.
    /// Off by default; the editor.remoteReferences setting drives it. Off,
    /// a remote name is a null icon and nothing is sent. Without network
    /// support in the build this does nothing.
    static void setRemoteAllowed(bool on);

    /// Drops every cached pixmap, and forgets which remote URLs already
    /// failed so they get one more try; call after a theme change.
    static void clearCache();

#ifdef NOTEAPPC_HAVE_NETWORK
    /// The signal source for a remote fetch completing; see IconsNotifier.
    static QObject *notifier();
#endif
};

} // namespace noteAppC
