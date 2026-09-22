# noteAppC handoff

Written 2026-09-22, so a cold start can pick this up without reading the
transcript. Kept current: everything below is true as of 2026-09-22, with the
callout and performance work staged (see `TODO.md` for what is in flight).

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

All 9 test suites pass and the full build is clean.

```
vault  markdown  index  editor  window  theme  icons  settings
```

Measured on starbase-1, 2 000-note vault, with `tools/bench/noteAppC-bench`:
cold start 199 ms, note open 1.4 / 9.4 / 53 ms at 1 / 50 / 200 kB, keystroke
0.29 ms at 200 kB against an 8 ms budget, scroll frame 0.57 ms median, peak
RSS 201 MB. A 1 MB note still opens in 822 ms, which is the next number worth
attacking.

Working today:

- **Opening**: a vault, or a loose `.md` in single-file mode with no vault, no
  index and no dotfile. A note inside a folder that has `.obsidian` opens the
  whole vault.
- **Shell**: noteAppC's own. Header bar with vault menu and a centred search
  that opens the quick switcher, flat underline tabs, file tree with new note,
  new folder, rename and delete, right sidebar with backlinks, tags and
  outgoing links, status bar. A settings page with live-applied values, Reset
  per row and a "set by this vault" tag.
- **Callouts**, which are noteAppC's own beyond Obsidian's two CSS knobs:
  named styles in `~/.config/noteAppC/callouts.ini` (icon, both colours with
  alpha, both fonts, both sizes, both colours, font effects, `inherits`),
  overridable per callout in Obsidian's `|` metadata slot so notes stay
  portable. They nest, and a fenced listing inside one keeps its font, its
  panel and its highlighting.
- **Maths**: `$$...$$` through MicroTeX, drawn as outlines so every large
  operator, radical, delimiter and matrix bracket appears. Inline `$...$` is
  parsed and styled but **not yet rendered**.
- **Note rendering**, which is the part that follows Obsidian: live preview,
  source and reading modes, headings, emphasis, highlight, links and
  wikilinks with unresolved ones dimmed, lists, checkboxes, tables as a grid,
  callouts, `:::` containers, image embeds, fenced code with syntax
  highlighting and `#|` options, the properties table, folding.
- **Editing**: auto-pairs, list continuation and renumbering, Tab indent,
  checkbox toggle, line move and duplicate, formatting toggles, find and
  replace with regex.
- **Underneath**: md4qt with an Obsidian source pass, SQLite/FTS5 index with
  Obsidian's link resolution and ignore filters, byte-exact atomic save,
  rename that rewrites every link, custom colour schemes with an importer for
  Obsidian themes installed in the vault.

## Next, in order

### 1. Settings that nothing applies yet

The settings page shows these and stores them, but no code consumes them.
Each needs a setter on `MarkdownEditor` or `Theme` first; `SettingsPage.cpp`
carries a `// TODO` at each one.

`editor.strictLineBreaks`, `editor.showIndentGuides`, `editor.tabWidth`,
`links.useWikilinks`, and `appearance.interfaceFont` / `textFont` /
`monospaceFont` (Theme has no family-override hook). `editor.spellcheck` has
no engine behind it at all - hunspell is in the plan and not yet wired.
`appearance.showRibbon` should just be deleted: the ribbon it gated is gone.

### 2. Two rough edges in Settings

- `Settings::load(vaultRoot)` throws away unsaved session state on a vault
  switch, so `MainWindow::reloadSettings()` hand-carries anything
  `isCustomised()` across the reload. A merge API on `Settings` would remove
  that workaround.
- `Settings` and `Vault` both read `.obsidian/app.json`, for different keys.

### 3. Property values other than the checkbox are read-only

Text, number, date and list rows in the properties table render but do not
edit. Each wants an inline typed editor writing back through `applyEdit` into
that property's own `range`, the way the checkbox already does. Anything the
editor cannot round-trip safely - a nested map, a quoted value - must stay
read-only rather than risk the file.

### 4. Known bugs and smaller jobs

- **Heading and block links show their raw anchor**: `Second note#Details`
  where Obsidian shows `Second note > Details`, and `Second note#^anchor`
  where Obsidian shows the block's text. Substituting the displayed text means
  the DisplayMap has to map a 1-character source range onto 3 displayed
  characters; today it only hides, so this needs a replacement run first.
- **`#|` highlight lines are clamped against a count that includes the option
  lines**, so a high line number in a short listing can be clamped away.
- **A `:::` container's range swallows the blank line after its fence**, so two
  adjacent containers or callouts have no gap between their panels.
- Property values other than the checkbox are read-only. Text, number, date
  and list want inline typed editors.

- **Reference links resolve wrong.** md4qt reports `[text][ref]` with a target
  of `#REF//<path>`, so the index records a link to a note literally named
  `ref` and the definition's real URL is dropped. Fix by collecting the
  definitions and resolving labels in `Parser::parseAll`.
- Two dangling-else warnings in `tests/test_markdown.cpp`.
- Block spacing in live preview is looser than Obsidian's; compare a
  screenshot side by side.
- Source mode gives a blank line its own tall gap because block spacing
  applies there too. Obsidian's source mode has uniform line height.
- Consolidating the two `.obsidian/app.json` readers was considered and
  deliberately deferred: it is one small JSON read each, and the ignore
  filters are tested and working. Do it when something else forces a change
  there, not for tidiness.

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
