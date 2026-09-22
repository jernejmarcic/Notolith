#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QString>

namespace noteAppC {

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
    /// http(s) URL ever fires this, and only once it is cached, never on
    /// failure. Call Icons::icon() again afterwards to pick up the result.
    void iconReady(const QString &name);
};

/// Lucide icons (ISC), the set Obsidian itself draws with, recoloured to a
/// theme token at load - and, in the same slot, a callout's own icon: a local
/// image file or a remote URL.
class Icons {
public:
    /// `name` is a Lucide id ("folder-open"), an absolute or `file://` path
    /// to a local .svg/.png/.jpg/.webp, or an `http://`/`https://` URL to the
    /// same. Every result is scaled to `size` and centred inside the square,
    /// preserving its own aspect ratio, so a photograph lines up with a glyph
    /// in the same column rather than stretching to fill it.
    ///
    /// `color` recolours only an SVG that spells `currentColor` - the Lucide
    /// set does, so today's callers are unaffected; a user's own image, SVG
    /// or raster, is drawn as-is and never tinted.
    ///
    /// Returns a null icon when the resource is missing, which shows up as a
    /// blank button rather than a crash. This is also what a remote URL
    /// returns before it is cached: this call never blocks on the network.
    /// The fetch starts in the background, lands in the XDG cache directory,
    /// and notifier()'s iconReady(name) fires once it is there, so the
    /// caller can repaint and call icon() again to pick it up. A fetch that
    /// fails is remembered so it is not retried on every paint.
    static QIcon icon(const QString &name, const QColor &color, int size = 18);

    /// Drops every cached pixmap, and forgets which remote URLs already
    /// failed so they get one more try; call after a theme change.
    static void clearCache();

    /// The signal source for a remote fetch completing; see IconsNotifier.
    static QObject *notifier();
};

} // namespace noteAppC
