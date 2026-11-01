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
/// A file moves by an atomic rename where it can, else by copy, fsync and
/// remove; one that already exists at its destination is never overwritten.
/// graph.json and the publish dialog's choices were keyed by the SHA-1 of a
/// vault's path; they are re-keyed through the recent-vaults list, and those of
/// a vault not in it are dropped (view state only).
///
/// Idempotent and cheap once done: a marker file in the state directory ends
/// every later call at one stat, and a machine with no old directory is left
/// untouched. Run it before the first Settings::load(). One info line per file
/// moved: qInfo(), or appended to `lines` when the caller has none to spare yet
/// (the log file is opened after this runs). Returns how many files were moved.
int migrateStorage(QStringList *lines = nullptr);

} // namespace noteAppC
