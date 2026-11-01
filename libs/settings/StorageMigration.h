#pragma once

#include <QStringList>

namespace noteAppC {

/// Moves what an earlier build kept in `noteAppC` directories to where the XDG
/// Base Directory spec puts it, under `notolith`:
///
///   config  ~/.config/noteAppC        -> ~/.config/notolith (settings, themes, hotkeys, plugin config...)
///   state   vaults.ini, noteAppC.conf (window geometry, as window.ini), epub-positions.ini,
///           workspace.json and graph.json per vault -> ~/.local/state/notolith
///   data    bookmarks.json, icons.json, workspaces.json per vault, and ~/.local/share/noteAppC
///           (recovery, history, sync, share, plugins) -> ~/.local/share/notolith
///   cache   ~/.cache/noteAppC is deleted, not moved: all of it is rebuilt on use
///
/// A file moves by an atomic rename where it can, else by copy, fsync of the
/// copy and of its directory, and remove; one that already exists at its
/// destination is never overwritten. A symbolic link moves as a link, never as
/// what it points at. A FIFO, a socket or a device is not the app's and is not
/// touched. graph.json and the publish dialog's choices were keyed by the SHA-1
/// of a vault's path; they are re-keyed through the recent-vaults list, and
/// those of a vault not in it keep the key they have.
///
/// A file that cannot be moved stays exactly where it is, with one warning line
/// naming it and why; a directory that is a symbolic link is left alone. Such
/// a run leaves an "incomplete" marker, and every later start tries again; a
/// run that leaves nothing behind writes a "complete" one, which ends every
/// later call at one read, and a machine with no old directory is left
/// untouched. Run it before the first Settings::load().
///
/// One info line per file moved goes to qInfo(), or is appended to `lines` when
/// the caller has none to spare yet (the log file is opened after this runs);
/// the warnings go to qWarning() or are appended to `warnings`, and the caller
/// has something to show when that is not empty. Returns how many files were moved.
int migrateStorage(QStringList *lines = nullptr, QStringList *warnings = nullptr);

/// What a start has to do about the old layout: nothing, the first migration, or
/// the retry of one that left files behind. Cheap: one read, and a stat per old directory.
enum class MigrationNeed { None, Fresh, Retry };
MigrationNeed storageMigrationNeed();

} // namespace noteAppC
