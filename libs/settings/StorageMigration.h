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
/// naming it and why. If another start may manage it (a disk that was full, a
/// directory that could not be made) the run leaves an "incomplete" marker and
/// every later start tries again. If something is in its way that only the
/// person can settle (a file at the destination, a FIFO, a directory that is a
/// symbolic link) it is said once, written under an "attention" marker with its
/// path and the reason, and left alone from then on: no later start tries it
/// or says it again. A run that leaves nothing behind writes a "complete"
/// marker, which ends every later call at one read, and a machine with no old
/// directory is left untouched. When the copy of a file is in place but the
/// original cannot be removed (a directory this user cannot write), it counts
/// as moved and the original is listed the same way. Run it before the first
/// Settings::load().
///
/// One info line per file moved goes to qInfo(), or is appended to `lines` when
/// the caller has none to spare yet (the log file is opened after this runs);
/// the warnings go to qWarning() or are appended to `warnings`, and the caller
/// has something to show when that is not empty. Returns how many files were moved.
int migrateStorage(QStringList *lines = nullptr, QStringList *warnings = nullptr);

/// What a start has to do about the old layout: nothing, the first migration, or
/// the retry of one that left files behind that a retry can move. Cheap: one read, and a stat per old directory.
enum class MigrationNeed { None, Fresh, Retry };
MigrationNeed storageMigrationNeed();

} // namespace noteAppC
