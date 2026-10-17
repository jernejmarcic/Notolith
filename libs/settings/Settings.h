#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <optional>

namespace noteAppC {

/// How the app was launched. It decides how much chrome there is, whether
/// anything is indexed, and whether a vault exists at all.
enum class OpenMode {
    Empty,      ///< nothing named: the last vault, or an empty window
    SingleFile, ///< one Markdown file, no vault, no index, no dotfile
    Vault,      ///< a directory of notes, the way Obsidian means it
};

/// What a command-line argument turned out to be.
struct LaunchTarget {
    OpenMode mode = OpenMode::Empty;
    QString filePath;  ///< absolute path of the note to open, may be empty
    QString vaultRoot; ///< empty in SingleFile mode
};

/// Works out what the user meant by `noteAppC <path>`.
///
/// A directory is a vault. A file is a single file - unless it sits inside a
/// folder that is already a vault (an ancestor holding `.obsidian`), because
/// opening a note from a file manager should give the whole app rather than a
/// lone window with no links. A folder in the recent-vaults list counts as a
/// vault the same way, `.obsidian` or not.
LaunchTarget resolveLaunchTarget(const QString &argument);

/// The application's settings, and the only place that decides where they
/// live.
///
/// The defaults are compiled in. The user's own settings are read from
/// `~/.config/noteAppC/noteAppC.conf` and are the only thing ever written, so
/// running the app on a stray Markdown file leaves nothing behind. A vault may
/// layer Obsidian's `app.json` and `appearance.json` on top, read-only, so an
/// existing vault keeps its behaviour without being converted to anything.
class Settings {
public:
    /// Defaults plus the user's config. `vaultRoot` adds the vault layer.
    static Settings load(const QString &vaultRoot = {});

    /// The configuration directory, `~/.config/noteAppC`, created only when
    /// something is actually saved. Its shape:
    ///
    ///     ~/.config/noteAppC/
    ///       settings.ini    what a person sets, grouped and commented
    ///       vaults.ini      what the app remembers: known vaults, last note
    ///       themes/         colour schemes, one file each
    ///       templates/      note templates
    ///
    /// Settings and memory are kept apart on purpose: `settings.ini` is meant
    /// to be opened in an editor, and it would be unreadable if the app
    /// rewrote it every time a note was opened.
    static QString configDirectory();
    static QString configFile();
    /// Where the app's own memory goes, never the settings file.
    static QString indexFile();
    static QString themesDirectory();
    static QString templatesDirectory();

    QVariant value(const QString &key) const;
    bool boolean(const QString &key) const { return value(key).toBool(); }
    int integer(const QString &key) const { return value(key).toInt(); }
    QString text(const QString &key) const { return value(key).toString(); }

    /// Sets a value in the user layer. Nothing reaches disk until save().
    void setValue(const QString &key, const QVariant &value);
    /// True when the value differs from the compiled-in default.
    bool isCustomised(const QString &key) const;
    /// Drops the user's value, so the default (or the vault's) applies again.
    void reset(const QString &key);

    /// Writes the user layer to `~/.config/noteAppC`. Never touches a vault.
    bool save(QString *error = nullptr) const;

    /// Every key, sorted, for the settings page to iterate.
    static QStringList keys();
    /// The compiled-in default, for "reset to default" and for save filtering.
    static QVariant defaultValue(const QString &key);

    /// True when this key came from the vault's Obsidian config rather than
    /// from the user - the settings page shows those as "set by this vault".
    bool isFromVault(const QString &key) const { return m_vault.contains(key); }

    QString vaultRoot() const { return m_vaultRoot; }

    /// Recently opened vaults and files, most recent first.
    QStringList recentVaults() const;
    QStringList recentFiles() const;
    void noteOpened(const QString &vaultRoot, const QString &filePath);

    /// A note's caret offset and vertical scroll pixel, as last recorded by
    /// setNotePosition().
    struct NotePosition {
        qint64 cursor = 0;
        int scroll = 0;
    };
    /// Unset when `path` was never recorded, or has aged out of the cap.
    std::optional<NotePosition> notePosition(const QString &path) const;
    /// Remembers where `path` was left, evicting the oldest entry once more
    /// than 50 notes are tracked. Written to `vaults.ini` by save(), the same
    /// as recentVaults/recentFiles - never settings.ini, never the vault.
    void setNotePosition(const QString &path, qint64 cursor, int scroll);
    /// Every note with a remembered position, most recently visited first.
    QStringList rememberedNotes() const;

    /// A feature that can be switched off, costing nothing once it is: an
    /// Obsidian core plugin, by its id in `.obsidian/core-plugins.json`, or
    /// one of noteAppC's own (`obsidian` false, never read from a vault).
    struct Feature {
        const char *id; ///< "global-search"
        const char *name;
        const char *description;
        bool obsidian;
    };
    /// Every feature, in the order the Core plugins page lists them.
    static const QList<Feature> &features();
    /// Where a feature's switch lives: "note-history" is
    /// "plugins.noteHistory", camelCase being the only form settings.ini
    /// reads back.
    static QString featureKey(const char *id);

    /// Reads Obsidian's own `app.json`, `appearance.json` and
    /// `core-plugins.json` from a vault and maps them onto noteAppC's keys. Public so the tests can check the
    /// mapping without a whole vault on disk.
    static QVariantMap fromObsidianConfig(const QString &vaultRoot);

private:
    QVariantMap m_user;  ///< what the user changed, written to settings.ini
    QVariantMap m_index; ///< what the app remembers, written to vaults.ini
    QVariantMap m_vault; ///< what the vault's Obsidian config says
    QString m_vaultRoot;
};

} // namespace noteAppC
