# Notolith

A fast, native Markdown notes app for Linux that opens Obsidian vaults as they
are. C++20 and Qt 6 Widgets: no JavaScript, no browser engine, no Electron.

(Code name noteAppC: the repository's targets, the C++ namespace and the
directories where it keeps settings and caches still carry it. The binary is
`notolith` and the app id is `io.github.jernejmarcic.Notolith`.)

## Features

- **Vault** - open any folder of Markdown files, with a file tree and
  byte-exact atomic saves (CRLF and BOM preserved). File recovery keeps
  snapshots with a diff. A note changed on disk (sync, another editor, a link
  update) shows the change in every tab; if you were editing it, your edits
  win and the other version goes to file recovery. A note moved or renamed
  outside the app is followed, caret and unsaved edits kept.
- **Index** - SQLite with FTS5, parallel cold build, incremental rescan,
  Obsidian's link resolution order. 20 000 notes cold-index in about 1 s.
- **Editor** - one engine, three modes: live preview, source and reading.
  Viewport-only layout, incremental edits, parsing off the UI thread; 0.2 ms
  per keystroke on a 200 kB note.
- **Rendered Markdown** - Obsidian's element-by-element syntax reveal,
  callouts, tables, maths, embeds of notes, images, PDFs, audio and video,
  editable properties, `query` blocks, raw HTML (litehtml), Mermaid, PlantUML,
  TikZ and Graphviz diagrams, and syntax-highlighted code.
- **Vim mode** - optional: normal, insert and visual modes, counts, operators
  and motions, `.`, `/` search, `:w` and `:q`.
- **Workspace** - tabs and nested splits, each tab with its own history; the
  layout comes back with the vault, and named layouts can be saved and loaded.
- **Core plugins** - search with Obsidian's grammar, backlinks and unlinked
  mentions, outgoing links, tags, outline, bookmarks, daily notes, templates,
  page preview, PDF export, pandoc export and import, hotkeys, a command
  palette (Ctrl+P), and a right-click menu with a web search.
- **Modules** - spellcheck, graph view (global and local), canvas (JSON
  Canvas), bases (`.base` views), a PDF viewer with annotation, image
  annotation, handwritten notes with pen input, code blocks run through a
  Jupyter kernel, Git-backed note history, and static-site publishing. Each
  can be switched off at build time.
- **Offline** - the default build has no networking and no encryption. Sync,
  note sharing, encrypted notes, the AI plugin, word lookup and remote content
  are not in the default build: see `docs/OFFLINE.md`.
- **Plugins** - out-of-process plugins, sandboxed with bubblewrap.
- **Themes** - dark by default, Obsidian's token names and values, custom
  colour schemes, an importer for any Obsidian theme in the vault, and
  per-kind formatting colours (headings, bold, links, ...; display only,
  nothing is written to a note).
- **Touch** - momentum scrolling and long-press selection; sidebars fold on a
  narrow window.

## Install

### Arch Linux and CachyOS

```sh
git clone https://github.com/jernejmarcic/Notolith
cd Notolith/packaging/arch
makepkg -si --nocheck
```

This builds and installs four packages together: `notolith`, `notolith-ink`,
`notolith-hwfont` and `notolith-pdfview`. The first build needs network access
to fetch its sources (see the header of `packaging/arch/PKGBUILD`); the
installed app uses none.

Update: `git pull`, then `makepkg -sif --nocheck` in `packaging/arch`.

Uninstall:

```sh
sudo pacman -Rns notolith notolith-ink notolith-hwfont notolith-pdfview
```

### Other Linux: build from source

Needs Qt 6.7 or newer (`qt6-base`, `qt6-svg`, `qt6-multimedia`), a C++20
compiler, CMake 3.28 or newer and Ninja. The list below uses Arch package
names; most are optional and switched off cleanly when missing.

- Always: `qt6-base` `qt6-svg` `yaml-cpp` `cmake` `ninja` `git`
- Optional modules: `qt6-multimedia` `hunspell` `syntax-highlighting`
  `litehtml` `libgit2` `libarchive` `zeromq` `libseccomp` `abseil-cpp`
  `poppler-qt6`
- At run time: `bubblewrap` (sandboxes code blocks, TeX and plugins)
- Tests: `gtest`

md4qt, MicroTeX, tinyxml2 and ink-stroke-modeler are fetched at configure time.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Flatpak and AppImage recipes are in `packaging/` (untested).

## Running

```sh
notolith                 # opens the last vault
notolith ~/notes         # a vault
notolith note.md         # one file, no vault
```

Optional tools, used only when present, for rendering diagrams and export:
`mmdr` (`cargo install mermaid-rs-renderer`) for Mermaid, `plantuml`, TeX Live
(`latex` and `dvisvgm`) for TikZ, `graphviz` for Graphviz, and `pandoc`. They
run sandboxed with bubblewrap, off the UI thread. See `docs/EDITOR.md` and
`docs/PANDOC.md`.

## Development

```sh
cmake -B build -G Ninja
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

`sh tools/check.sh` builds and tests the default configuration, one with every
networked and encrypting option on, and one with every optional module off.

Documentation, in `docs/`:

- `EDITOR.md` - the editor, HTML editing, vim mode, diagram rendering
- `SEARCH.md` - the search grammar
- `STYLING.md` - callout and code-block styles
- `GRAPH.md`, `CANVAS.md` - graph view and JSON Canvas
- `ANNOTATIONS.md` - image annotation
- `INK.md`, `INK-DESIGN.md` - handwritten notes: usage and design
- `NOTEBOOKS.md` - running code blocks
- `OFFLINE.md` - what the default build leaves out, and the options that bring it back
- `ENCRYPTION.md` - encrypted notes (not in the default build)
- `SYNC.md` - end-to-end encrypted sync (not in the default build)
- `SHARE.md` - peer-to-peer note sharing (not in the default build)
- `PLUGINS.md` - the plugin system
- `PANDOC.md` - pandoc export and import
- `PERFORMANCE.md` - measurements and what was tried
- `PLATFORMS.md` - supported platforms and what portability requires

## License

GPL-3.0-or-later, see `LICENSE`. Bundled: md4qt and MicroTeX (MIT), tinyxml2
(zlib), Lucide icons (ISC).
