# noteAppC handoff

Written 2026-09-22 at the end of a session, so a cold start can pick this up
without reading the transcript. Everything below is true of commit `679e0ae`.

## What this is

A Linux-native alternative to Obsidian in C++ with Qt 6 Widgets. No
JavaScript, no QML, no browser engine, no Electron.

- Plan and feature inventory:
  `~/Documents/AI/sessions/2026-09-22_011551_obsidian-clone-cpp-plan/`
  (`FEATURES.md` is what Obsidian does, `PLAN.md` is how this gets built).
- Session records:
  `~/Documents/AI/sessions/2026-09-22_013550_obsidian-clone-phase0/` (phases 0
  to 2) and `~/Documents/AI/sessions/2026-09-22_103114_noteappc-phase1-3-parallel/`
  (the parallel tracks, and every decision made since).

## Constraints that are not negotiable

1. **C and C++ only.** No JavaScript anywhere, which is also why Qt Widgets
   and not QML: the QML binding engine is a JS engine. No QtWebEngine.
2. **The file is the truth.** Never rewrite bytes that were not edited. CRLF
   and a BOM survive a round trip. `Vault::read`/`write` exist for this.
3. **Never create a dotfile.** Settings live in `~/.config/noteAppC`, the
   metadata cache in XDG cache keyed by vault path. A vault's `.obsidian` is
   read when present, written only if the user explicitly asks.
4. **The imitation stops at the edge of the note.** Note rendering follows
   Obsidian closely, because that is the thing being replaced. The shell is
   noteAppC's own. This changed mid-session; see "Next" below.
5. **Performance budget**, asserted in `tests/test_editor.cpp` so a regression
   fails the build: 8 ms per keystroke and 300 ms to open on a 200 kB note.
   Currently 0.09 ms and 72 ms.
6. Naming is modified camelCase: initialisms stay uppercase (`noteID`,
   `JSONdata`, `HTMLblock`), never `noteId`.

## State

All 7 test suites pass and the full build is clean.

```
vault  markdown  index  editor  window  theme  settings
```

Working today: opening a vault or a single file, the file tree, byte-exact
atomic save, md4qt parsing with an Obsidian source pass, the SQLite/FTS5 index
with backlinks and Obsidian's link resolution, the editor engine in live
preview, source and reading modes, inline rendering of headings, emphasis,
highlight, links, wikilinks, lists, checkboxes, tables as a grid, callouts,
image embeds and fenced code with syntax highlighting, the right sidebar's
backlinks, tags and outgoing-links panes, quick switcher, `[[` autocomplete,
custom colour schemes with an Obsidian theme importer, file operations with
rename-updates-links, and the editing behaviours (auto-pairs, list
continuation, find and replace).

## Next, in order

### 1. Shell redesign and settings page (specified, not started)

An agent was dispatched with the spec below and stopped before writing
anything, so nothing is half-done. Owns `libs/ui/*`, `apps/noteAppC/main.cpp`,
`tests/test_window.cpp`.

- **Replace the ribbon with a top header bar**, 44 px, object name
  `headerBar`. Left: sidebar toggle (`panel-left`), then the vault name as a
  menu button (Open folder, Open recent from `Settings::recentVaults()`, Close
  vault). Centre: a rounded search field `headerSearch`, max 420 px, muted
  `--background-modifier-form-field`, placeholder "Search or jump to...",
  which opens the existing `QuickSwitcher` beneath it on focus or Ctrl+O.
  Right: new note (`plus`), right sidebar toggle (`panel-right`), settings
  (`settings`). Delete `buildRibbon`.
- **Flat tabs**: no tab background, label `--text-muted`, active
  `--text-normal` with a 2 px `--interactive-accent` underline, close on
  hover. Same language for the right sidebar's tabs, which become icon plus
  label (`link` Backlinks, `tags` Tags, `arrow-up-right` Outgoing - all three
  icons now exist; 46 are registered in `libs/theme/CMakeLists.txt`).
- **Left sidebar** 260 px, an 11 px uppercase "FILES" label row in
  `--text-faint` with `file-plus` and `folder-plus` buttons right-aligned in
  it, and a 6 px rounded accent-tinted selection instead of a flat bar.
- **Settings page**, object name `settingsPage`: a page in the main area, not
  a modal. 200 px left nav (Appearance, Editor, Files and links, About) beside
  scrolling rows of label plus muted description on the left and control on
  the right, hairline between. Opened by the header button or Ctrl+comma,
  closed by Escape, restoring the previous note. Reads and writes through
  `noteAppC::Settings`, applies live, `save()` on close. A row differing from
  `Settings::defaultValue(key)` offers "Reset"; a row where
  `Settings::isFromVault(key)` is true is tagged "set by this vault". The
  scheme control lists `Scheme::available(vaultRoot)` plus "Built-in", applies
  with `Scheme::apply(path, mode)`, and tags an Obsidian theme as such. Write a
  custom painted `ToggleSwitch` rather than using `QCheckBox`.
- **Single-file mode**: `main.cpp` calls `resolveLaunchTarget()` from
  `libs/settings` and hands `MainWindow` a `LaunchTarget`; add
  `MainWindow::openTarget()`. In `OpenMode::SingleFile` open no index at all,
  hide both sidebars and the tab strip, put the file name where the vault name
  goes plus an "Open folder as vault" action, keep the status bar.
  `OpenMode::Empty` opens the most recent vault or an empty window.
- **Apply settings on startup** in both modes: theme mode and scheme, accent,
  font sizes, and the editor setters that exist (`setMode`,
  `setReadableLineLength`, `setContentWidth`, `setLineNumbers`). Call
  `Settings::noteOpened()` on open and `save()` on close.

Settings with no editor setter yet, so they need one adding to
`MarkdownEditor` first or a `// TODO` leaving: auto-pairs (a setter may now
exist after the editing-behaviours commit - check), tab width, indent guides.

### 2. Wire the file operations into the UI

`Vault::createNote`, `createFolder`, `renamePath`, `moveToTrash` and
`Index::updateLinksAfterRename` all exist and are tested, but nothing calls
them. They want a file-tree context menu (new note, new folder, rename,
delete), the two header buttons, and a rename that goes through the index so
links follow.

### 3. Render what the parser now produces

The markdown layer emits these and the editor ignores them:

- `Decorations::codeOptions` - `#|` block options. Find a block's entry by
  `options.block == blockSpan.range`. Fields: `title`, `caption`,
  `lineNumbers`, `firstLine`, `highlight` (1-based, clamped), `wrap`, `fold`,
  `extra`. Render the title header, the line numbers, the highlighted rows.
- `:::` containers, which arrive in `decorations.callouts` with the whole
  opening fence line as `marker` - so the callout renderer already has them,
  but `details` (collapsed by default, `summary=`) needs handling.
- Typed frontmatter properties: `Property` now carries `type`, `raw` and
  `range`. Obsidian shows these as a properties table at the top of the note;
  today the frontmatter renders as a monospace block and leaves a blank gap in
  live preview. This is the most visible gap against Obsidian.

### 4. Known bugs and smaller jobs

- **The right sidebar panes miss the first index.** Open a vault with a cold
  cache (`rm -rf ~/.cache/noteAppC`) and the backlinks pane says 0 for a note
  that has backlinks; a warm cache hides it. The index itself is right -
  `noteAppC-index --fresh fixtures/demo-vault Welcome` reports 2. The panes
  need to re-resolve the active note when `Index::changed` fires.
- **Unresolved links look like resolved ones.** `[[Nowhere]]` renders in the
  same accent as a real link. Obsidian dims them, which is how you spot a typo
  in a link. The editor already has a link resolver; style on its answer.
- **Heading and block links show their raw anchor**: `Second note#Details`
  where Obsidian shows `Second note > Details`, and `Second note#^anchor`
  where Obsidian shows the block's text.

- **Reference links resolve wrong.** md4qt reports `[text][ref]` with a target
  of `#REF//<path>`, so the index records a link to a note literally named
  `ref` and the definition's real URL is dropped. Fix by collecting the
  definitions and resolving labels in `Parser::parseAll`.
- Two dangling-else warnings in `tests/test_markdown.cpp`.
- Block spacing in live preview is looser than Obsidian's; compare a
  screenshot side by side.
- Source mode gives a blank line its own tall gap because block spacing
  applies there too. Obsidian's source mode has uniform line height.
- `Settings` and `Vault` both read `.obsidian/app.json` - `Settings` for the
  editor keys, `Vault` for `userIgnoreFilters`. Consolidating on `Settings`
  was considered and deliberately deferred: the duplication is one small JSON
  read each, and the ignore filters are tested and working. Do it when
  something else forces a change there, not for tidiness.

### 5. Then the plan's remaining phases

MicroTeX math, PDF and media embeds, HTML blocks through litehtml, the search
grammar with `query` blocks, the graph view, Canvas, Bases. See `PLAN.md`
sections 6, 9, 10 and the phase table in 12.

## Performance, measured

The plan's budget is 20 000 notes cold-indexed in under 5 s. It is met, but
only after a fix: the full-text table was deleted by a non-indexed column, so
a cold build was quadratic.

| notes | before | after |
|---|---|---|
| 5 000 | 3.49 s | 0.98 s |
| 10 000 | 14.68 s | 1.57 s |
| 20 000 | 39.80 s | 3.57 s |

Reproduce it:

```sh
python3 tools/gen_bench_vault.py /tmp/bench-vault 20000
./build/tools/noteAppC-index --fresh /tmp/bench-vault
```

The editor's budgets are asserted in `tests/test_editor.cpp` and currently met
at 0.09 ms per keystroke and 72 ms to open a 200 kB note. The index has no such
assertion yet; Phase 7 wants one, and the generator above is its seed.

## Building and looking at it

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/apps/noteAppC/noteAppC <vault-or-file>
```

Render the window offscreen, which is how the look gets reviewed without a
display:

```sh
./build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800
./build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800 --source
```

`fixtures/demo-vault` is committed for exactly this: one note covering most of
the syntax, a second to link to and embed, and a `.obsidian` so the
vault-config path is exercised.

```sh
./build/tools/noteAppC_shot fixtures/demo-vault fixtures/demo-vault/Welcome.md out.png dark 1240 820
```

Use it rather than building a vault by hand, so the next person can retake the
same screenshot.

## Working with several agents in this tree

It works, with one rule: **ownership is by file and nothing overlaps.** The
split that worked was `libs/markdown`, `libs/editor`, `libs/vault`+`libs/index`,
`libs/ui`, each with its own test file, each building in its own
`build-<track>` directory, none of them running a git command. The parent
session owns `libs/theme`, `libs/settings`, `tools`, the root CMake,
`tests/CMakeLists.txt`, and every commit. Right-size the model per track:
parser and engine work needs the strong one, widgets and CRUD do not.
