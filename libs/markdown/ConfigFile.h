#pragma once

#include <QHash>
#include <QMap>
#include <QString>

namespace noteAppC {

/// `~/.config/noteAppC/<name>`, or under `$XDG_CONFIG_HOME` when it is set.
///
/// XDG straight from the environment rather than QStandardPaths, the same
/// rule Settings follows: QStandardPaths needs an application name and so
/// answers differently in a tool or a test than it does in the app.
QString configFilePath(const QString &name);

/// Reads a `[section] key = value` file the way callouts.ini and codeblocks.ini
/// are written: `;` and `#` comment lines, a trailing ` ; comment`, quoted
/// values, and section and key names lower-cased. One reader for every such
/// file, so they cannot come to disagree about what a line means. A file that
/// is not there reads as empty - for these files that is the normal case.
QHash<QString, QMap<QString, QString>> readINIsections(const QString &path);

} // namespace noteAppC
