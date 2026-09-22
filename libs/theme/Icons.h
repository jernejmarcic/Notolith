#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace noteAppC {

/// Lucide icons (ISC), the set Obsidian itself draws with, recoloured to a
/// theme token at load. `currentColor` in the SVG source is the hook.
class Icons {
public:
    /// `name` is the Lucide id, e.g. "folder-open". Returns a null icon when
    /// the resource is missing, which shows up as a blank button rather than
    /// a crash.
    static QIcon icon(const QString &name, const QColor &color, int size = 18);

    /// Drops every cached pixmap; call after a theme change.
    static void clearCache();
};

} // namespace noteAppC
