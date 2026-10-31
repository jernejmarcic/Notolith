#pragma once

#include <QSet>
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
    ///
    /// Several Notolith processes (one per vault) share those files, each
    /// holding the settings it loaded. So a save writes what this object changed since
    /// it loaded (or last saved) on top of what is on disk now: another window's
    /// changes to other settings survive, a vault one window opened joins the recent
    /// vaults another saved, and a note position forgotten stays forgotten. A setting both
    /// changed takes the later save. The read and the write happen under a lock file (waited for `lockWaitMs` at most; a save from the UI thread
    /// on a note switch passes a few ms, not to stall it, and merges unlocked beyond that).
    /// What the file holds afterwards is what this object holds too.
    bool save(QString *error = nullptr, int lockWaitMs = 200);

    /// The settings a save took over from another window's write, since the last call: keys this object had
    /// not changed that now hold a different value. The window re-applies them; nothing else would.
    QStringList takeAdoptedKeys();

    /// A list of file types as typed ("PNG, .tar.gz *.pdf"): split on commas,
    /// semicolons and spaces, lower-cased, leading "*" and "." dropped,
    /// duplicates and empties removed. What `files.hiddenExtensions` holds.
    static QStringList fileTypes(const QStringList &raw);

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
        QStringList folds; ///< MarkdownEditor::foldState(), kept as given
        /// The source offset of the line at the top of the viewport, -1 for an entry
        /// from before it was kept: the scroll pixel only means the same text in the
        /// layout it was taken in (MarkdownEditor::topOffset()).
        qint64 top = -1;
    };
    /// Unset when `path` was never recorded, or has aged out of the cap.
    std::optional<NotePosition> notePosition(const QString &path) const;
    /// Remembers where `path` was left, evicting the oldest entry once more
    /// than 50 notes are tracked. Written to `vaults.ini` by save(), the same
    /// as recentVaults/recentFiles - never settings.ini, never the vault.
    void setNotePosition(const QString &path, qint64 cursor, int scroll, const QStringList &folds = {},
                         qint64 top = -1);
    /// A renamed or moved note, or every note under a renamed folder, keeps
    /// its position and folds under the new path.
    void renameNotePosition(const QString &oldPath, const QString &newPath);
    /// Forgets a deleted note, or every note under a deleted folder.
    void forgetNotePosition(const QString &path);
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

    /// A loadable module's own defaults and feature switch, registered when
    /// the module is discovered (after first paint, on the UI thread). Live:
    /// every Settings reads them from then on, and save() persists them.
    /// Registering a key or a feature twice is a no-op.
    /// The tables behind registerDefaults() and registerFeature() (defaults(), features()) are mutated on the UI thread only: register from there, never from a worker.
    static void registerDefaults(const QVariantMap &defaults);
    static void registerFeature(const QString &id, const QString &name, const QString &description);
    /// Load() ran before the keys of a module existed, so their stored lines
    /// were skipped. This re-reads settings.ini and vaults.ini for the keys
    /// registered since; a key this object already holds keeps its value.
    void adoptStored();

    /// Reads Obsidian's own `app.json`, `appearance.json` and
    /// `core-plugins.json` from a vault and maps them onto noteAppC's keys. Public so the tests can check the
    /// mapping without a whole vault on disk.
    static QVariantMap fromObsidianConfig(const QString &vaultRoot);

private:
    QVariantMap m_user;  ///< what the user changed, written to settings.ini
    QVariantMap m_index; ///< what the app remembers, written to vaults.ini
    QVariantMap m_baseUser;  ///< m_user as loaded or last saved: what save() tells "changed here" by
    QVariantMap m_baseIndex; ///< m_index likewise
    QStringList m_adopted;   ///< what save() took over from another window since takeAdoptedKeys()
    QSet<QString> m_touched; ///< remembered lists noteOpened() acted on since the last save
    QVariantMap m_vault; ///< what the vault's Obsidian config says
    QString m_vaultRoot;
};

} // namespace noteAppC
