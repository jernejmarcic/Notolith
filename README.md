# noteAppC

A Linux-native alternative to Obsidian, written in C++ with Qt 6 Widgets.
No JavaScript, no browser engine, no Electron.

Plan and feature inventory:
`~/Documents/AI/sessions/2026-09-22_011551_obsidian-clone-cpp-plan/`.

## Status

Usable for reading and editing a vault, and most of Obsidian is here. The
command palette is the one large piece deferred; `TODO.md` has the rest.

- **Vault** - open, file tree, byte-exact atomic save (CRLF and BOM preserved),
  file recovery snapshots with a diff.
- **Index** - SQLite with FTS5, parallel cold build, incremental rescan,
  Obsidian's link resolution order. 20 000 notes cold-indexed in 3.6 s.
- **Editor** - one engine, three modes: live preview, source and reading.
  Viewport-only layout, incremental edits, parse off the UI thread. 0.2 ms
  per keystroke on a 200 kB note, against an 8 ms budget.
- **Rendered** - Obsidian's element-by-element syntax reveal, callouts,
  tables, maths, embeds of notes, images, PDFs, audio and video, editable
  properties, `query` blocks, raw HTML (litehtml), syntax-highlighted code.
- **Vim mode** - optional (Settings > Editor): normal, insert and visual
  modes, counts, operators and motions, `.`, `/` search, `:w` and `:q`.
  `docs/EDITOR.md`.
- **Workspace** - tabs and nested splits, each tab with its own history; the
  layout comes back with the vault, and named layouts save and load.
- **Core plugins** - search with Obsidian's grammar, backlinks and unlinked
  mentions, outgoing links, tags, outline, bookmarks, daily notes,
  templates, page preview, PDF export, hotkeys.
- **Modules** - spellcheck, graph view (global and local), canvas (JSON
  Canvas), bases (`.base` views), each optional at build time.
- **Themes** - dark by default, Obsidian's token names and values, custom
  colour schemes, and an importer for any Obsidian theme in the vault.
- **Touch** - momentum scrolling and long-press selection on a touchscreen;
  sidebars fold on a narrow window. An Android build is in progress.

Never: plugins in JavaScript, Sync, Publish.

## Build

Build under `/tmp`, not in the project: `~/Documents` is synced to every
machine, and a build directory is gigabytes nobody needs copied.

```sh
sh tools/check.sh                         # everything: both configurations, every suite
cmake -B /tmp/noteAppC-build -G Ninja     # once, and again only if CMakeLists changes
cmake --build /tmp/noteAppC-build         # after every edit - this is the one you repeat
ctest --test-dir /tmp/noteAppC-build      # all seventeen suites, about twenty seconds
/tmp/noteAppC-build/apps/noteAppC/noteAppC            # opens the last vault
/tmp/noteAppC-build/apps/noteAppC/noteAppC ~/notes    # a vault
/tmp/noteAppC-build/apps/noteAppC/noteAppC note.md    # one file, no vault, no dotfile
```

`cmake --build /tmp/noteAppC-build` is incremental and takes a second or two when little
changed, so run it before every launch. Running an old binary looks exactly
like a feature not working.

Requires `qt6-base`, `qt6-svg`, `yaml-cpp`, `cmake`, `ninja`, a C++20
compiler, and `gtest` for the tests. Optional, each switched off cleanly when
missing: `qt6-multimedia`, `hunspell`, `poppler-qt6`, `syntax-highlighting`,
`litehtml`. md4qt, MicroTeX and tinyxml2 are fetched at configure time.
Packaging recipes (Arch, Flatpak, AppImage) are in `packaging/`.

Render the window to a PNG without a display, which is how the look is
reviewed:

```sh
/tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 1180 760
```
