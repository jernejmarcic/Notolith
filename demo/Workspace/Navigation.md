---
tags: [demo, workspace]
---

# Navigation

What to look for: the keyboard routes around a vault. Command palette,
quick switcher, bookmarks, workspaces, tabs and the two "pick a note" commands.
If a command below is missing from the palette, switch its plugin on in
Settings > Core plugins.

## Command palette and quick switcher

- Ctrl+P (`command-palette:open`) lists every command by name, with its key.
  Type part of "reading" and "Toggle reading view" is the match.
- Ctrl+O (`switcher:open`) fuzzy-finds a note by path. Type a name that does
  not exist and press Enter: a note with that name is made and opened. A `/` in
  the name makes the folders too.
- Ctrl+Shift+F opens vault search (`global-search:open`), see [[Search Operators]].

## Keys this vault rebinds

`.obsidian/hotkeys.json` holds two rebinds: Ctrl+Alt+V splits the tab to the
right, Ctrl+Shift+D opens today's daily note. Check them under Settings >
Hotkeys, where every command can be searched and rebound.

## Bookmarks

`.obsidian/bookmarks.json` seeds the Bookmarks pane in the right sidebar:
a note ([[Welcome]]), a heading of [[Callouts]], a saved search
(`tag:#embeds`) and a group named Reference. Run "Bookmarks: Bookmark this
note" to add this note. Right-click the pane for "New group", "Rename group" and
"Remove". Changes are kept in the app's own copy, not written to the vault.

## Tabs, splits and history

- Ctrl+T new tab, Ctrl+W close, Ctrl+Shift+T undo close, Ctrl+Tab next tab,
  Ctrl+1 to Ctrl+8 jump to a tab, Ctrl+9 the last one.
- "Toggle pin" keeps a tab from closing by accident.
- "Split right" and "Split down" open this note beside itself.
- Alt+Left and Alt+Right go back and forward through a tab's history.
- Windows are covered in [[Pop-out Windows]].

## Workspaces

Settings > Core plugins > Workspaces. `.obsidian/workspaces.json` holds two
layouts. Run "Workspaces: Load workspace layout" and pick Writing, then
"Workspaces: Manage workspace layouts" to save the current arrangement under a
new name.

## Random note and unique note

- "Random note: Open random note" opens any other note in the vault.
- "Unique note creator: Create new unique note" makes a note named by the
  date and time (`YYYYMMDDHHmm` unless `.obsidian/zk-prefixer.json` or Settings
  says otherwise), for a Zettelkasten.

Both plugins are off in this vault's `core-plugins.json`; switch them on in
Settings > Core plugins to see the commands.
