# noteAppC

A Linux-native alternative to Obsidian, written in C++ with Qt 6 Widgets.
No JavaScript, no browser engine, no Electron.

Plan and feature inventory:
`~/Documents/AI/sessions/2026-09-22_011551_obsidian-clone-cpp-plan/`.

## Status

Usable for reading and editing a vault. Phases 0 to 2 of the plan are done and
Phase 3 is under way.

- **Vault** - open, file tree, byte-exact atomic save (CRLF and BOM preserved).
- **Markdown** - md4qt with a source pass for Obsidian's own syntax, so every
  offset stays exact.
- **Index** - SQLite with FTS5, parallel cold build, incremental rescan,
  Obsidian's link resolution order, backlinks. 1638 notes in 0.76 s.
- **Editor** - one engine, three modes: live preview, source and reading.
  Viewport-only layout, incremental edits, debounced parse off the UI thread.
  0.09 ms per keystroke on a 200 kB note, against an 8 ms budget.
- **Rendered inline** - headings, emphasis, highlight, links and wikilinks,
  lists, checkboxes, tables as a grid, callouts, image embeds, fenced code
  with syntax highlighting, line numbers in source mode.
- **Shell** - Obsidian's chrome: ribbon, file explorer, tab strip, inline
  title, status bar, backlinks pane, quick switcher, `[[` autocomplete.
- **Themes** - dark by default, Obsidian's own token names and values, custom
  colour schemes, and an importer for the palette of any Obsidian theme
  already installed in the vault.

Not yet: graph view, canvas, search grammar, HTML blocks, media embeds. Never: plugins in JavaScript,
Sync, Publish.

## Build

Build under `/tmp`, not in the project: `~/Documents` is synced to every
machine, and a build directory is gigabytes nobody needs copied.

```sh
cmake -B /tmp/noteAppC-build -G Ninja     # once, and again only if CMakeLists changes
cmake --build /tmp/noteAppC-build         # after every edit - this is the one you repeat
ctest --test-dir /tmp/noteAppC-build      # all ten suites, about six seconds
/tmp/noteAppC-build/apps/noteAppC/noteAppC            # opens the last vault
/tmp/noteAppC-build/apps/noteAppC/noteAppC ~/notes    # a vault
/tmp/noteAppC-build/apps/noteAppC/noteAppC note.md    # one file, no vault, no dotfile
```

`cmake --build /tmp/noteAppC-build` is incremental and takes a second or two when little
changed, so run it before every launch. Running an old binary looks exactly
like a feature not working.

Requires `qt6-base`, `yaml-cpp`, `cmake`, `ninja`, a C++20 compiler, and
`gtest` for the tests (`ctest --test-dir /tmp/noteAppC-build`). md4qt is fetched at
configure time.

Render the window to a PNG without a display, which is how the look is
reviewed:

```sh
/tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 1180 760
```
