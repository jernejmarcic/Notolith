#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// The one name every XDG directory of the application carries
/// (`$XDG_CONFIG_HOME/notolith`, `$XDG_DATA_HOME/notolith`, ...). Not taken
/// from QCoreApplication's organisation and application names, so a tool, a
/// test and the app all answer the same.
inline constexpr QLatin1String kAppDirName{"notolith"};

/// The XDG base directories an application owns a directory in.
enum class XDGroot { Config, Data, State, Cache, Runtime };

/// The base directory itself. The environment variable only when it is an
/// absolute path (the spec says a relative one is invalid and ignored), else
/// the spec's default under $HOME: `~/.config`, `~/.local/share`,
/// `~/.local/state`, `~/.cache`. Runtime has no default: an absolute
/// `$XDG_RUNTIME_DIR`, else what Qt makes of it, else empty - callers treat
/// empty as "no runtime directory" and fall back on their own.
QString xdgRoot(XDGroot root);

/// `<xdgRoot>/notolith[/sub]`; empty when xdgRoot is.
QString appDir(XDGroot root, const QString &sub = {});

/// The search path of a base directory, most important first: the user's
/// directory, then each absolute `$XDG_CONFIG_HOME`/`$XDG_DATA_DIRS` entry
/// (the spec's `/etc/xdg` and `/usr/local/share:/usr/share` when unset). Only
/// Config and Data have one. These are the roots, without the application name.
QStringList xdgSearchDirs(XDGroot root);

/// Creates `path` and every missing parent with mode 0700, the spec's mode for
/// a directory it has to create. A directory that is already there is left as
/// it is. False when it cannot be made.
bool ensurePrivateDir(const QString &path);

/// The key of a vault in every per-vault path: 16 hex of the SHA-256 of its
/// absolute path.
QString vaultDigest(const QString &root);

/// `$XDG_CONFIG_HOME/notolith/<name>`.
QString configFilePath(const QString &name);

/// Reads a `[section] key = value` file the way callouts.ini and codeblocks.ini
/// are written: `;` and `#` comment lines, a trailing ` ; comment`, quoted
/// values, and section and key names lower-cased. One reader for every such
/// file, so they cannot come to disagree about what a line means. A file that
/// is not there reads as empty - for these files that is the normal case.
QHash<QString, QMap<QString, QString>> readINIsections(const QString &path);

} // namespace noteAppC
