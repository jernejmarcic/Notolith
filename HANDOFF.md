# noteAppC handoff

Written 2026-09-22, brought up to date 2026-09-23, so a cold start can pick
this up without reading a transcript. `TODO.md` is the live task list;
`docs/PERFORMANCE.md` is the measured record.

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
   fails the build: 8 ms per keystroke (a 200 kB note, a 5 000-item list and
   a 5 000-line code block, each after its parse has landed) and 300 ms to
   open a 200 kB note. Jernej's own rules on top: faster than Obsidian in
   every way; perceived speed never bought with more than 10 ms of real
   speed; nothing optional before the first paint; many CPUs, one core as the
   floor, a phone later; simplicity first, extras cost nothing when off.
6. **Build outside the tree.** `~/Documents` is shared by Syncthing, so a
   build directory in here is copied to every machine; builds go under
   `/tmp` (`/tmp/noteAppC-build` in every command below), and so do
   hand-made git worktrees.
7. Naming is modified camelCase: initialisms stay uppercase (`noteID`,
   `JSONdata`, `HTMLblock`), never `noteId`.

## State

All 26 suites pass in the full build and 15 in a build with every optional
module off (`sh tools/check.sh` runs both, as of 2026-09-23 evening).

Release build on starbase-1, 20 000-note vault, `tools/bench/noteAppC-bench`:
cold start to first painted frame 65 ms (including `openVault`; 42 ms with
every optional feature off), cold index 0.98 s, note open 1.5 / 8.2 / 3.3 /
21 ms at 1 / 50 / 200 / 1 024 kB and 3.7 ms for a vault note with every side
pane filled, keystroke 0.2-0.5 ms, a search 0.05-6.4 ms off the UI thread,
unlinked mentions 0.16 ms, peak memory 196 MB. Every fix behind the numbers,
with before and after, is in `docs/PERFORMANCE.md` (18 of them so far).

Working today, beyond the basics (vault or loose file, live preview / source /
reading, index, find and replace):

- **Tables** are edited where they are drawn (`libs/editor/TableCells.cpp`)
  and aligned in the source when left (`libs/markdown/TableFormat`); cell
  ranges come from `tableRowCells`, not md4qt, which misplaces empty cells.
- **Notebooks**: code blocks run through a Jupyter kernel
  (`libs/ext/notebook`, bubblewrap sandbox, output cache outside the note).
- **Obsidian's note rendering**: per-element syntax reveal, maths (MicroTeX),
  callouts, tables, embeds (notes, images, PDFs, audio, video), heading links,
  editable properties, `query` blocks, raw HTML through litehtml.
- **The workspace**: tabs, splits nested either way, each tab its own editor
  and history; the layout reopens with the vault and named layouts save and
  load, in Obsidian's `workspace.json` shape; background tabs load when shown.
- **Core plugins**: search with Obsidian's grammar, backlinks with unlinked
  mentions, outgoing links, tags, outline, bookmarks, daily notes, templates,
  page preview, file recovery, PDF export, hotkeys with a settings page.
- **Optional modules** (`libs/ext/`, each behind a CMake option, see
  [docs/PLUGINS.md](docs/PLUGINS.md)): spellcheck (hunspell), graph view
  (global and local), canvas (JSON Canvas), bases (`.base` views).
- **Styling beyond Obsidian**, in [docs/STYLING.md](docs/STYLING.md).
- **Packaging**: an Arch `PKGBUILD` that is built and checked, AppStream
  metadata; Flatpak and AppImage recipes not yet built (`packaging/`).
- **Platforms**: builds and passes on the Android kit's Qt 6.7.3 with the
  optional dependencies off; see [docs/PLATFORMS.md](docs/PLATFORMS.md).

### How the window holds its tabs

`MainWindow` owns `Leaf`s (one per tab: an editor, its inline title, the
note, its history, and optionally a canvas or base view in the editor's
place) inside `TabGroup`s (a tab bar over a stack) inside `QSplitter`s rooted
at `m_workspace`. `m_active` is the focused leaf; every command and side pane
follows it, and `activateLeaf()` is the one place that switches. A new editor
gets the current settings, theme, hotkeys and spelling from
`configureEditor()`. A restored tab keeps its file in `pendingPath` until it is
first shown.

### Traps this code has already fallen into

- `QRectF::united()` and `intersects()` treat a zero-width rect as empty: a
  point, or a straight vertical edge, silently drops out of a bounding box or
  a cull. Take bounds by hand; pad culling boxes.
- The editor styles an edited block at once and reparses the whole note 20 ms
  later, off the UI thread. A test that compares against a fresh load has to
  wait for that parse.
- Layout-driven work (query block searches, graph framing) only happens in a
  widget that has been shown; an offscreen test must `show()` the window.
- `findChildren<MarkdownEditor *>()` finds the page preview's editor too.
- Qt sends `qWarning` to the journal when stderr is not a terminal, so a
  debug print from a test run through a pipe seems to vanish.
- The same refresh wired from two signals costs twice: see fix 15.

## Next

`TODO.md` has the list. The ones that are Jernej's to answer:

- autosave like Obsidian, or the save prompt noteAppC has now;
- a licence, an app id, an app icon (packaging waits on all three);
- what Ctrl+click on a link should do (follow, as now, or a new tab as in
  Obsidian);
- three decisions in [docs/PLUGINS.md](docs/PLUGINS.md) (third-party
  plugins, Lua scripts, sidebar panes from plugins);
- run `bench-here.sh` from the brain's session directory on shuttlepod-1 and
  runabout;
- install a hunspell dictionary on starbase-1 (`hunspell-en_gb`);
- look in Obsidian at two behaviours nobody could confirm: whether the caret
  in a code block shows its fence lines, and how a block reference
  `[[Note#^id]]` is displayed.

## Performance, measured

**The full record is [docs/PERFORMANCE.md](docs/PERFORMANCE.md)**: current
numbers, every fix with its before and after, what was tried and rejected
(OpenGL, Vulkan), and the measurement pitfalls this project has already hit.
What follows is the original index finding, kept for its history.

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
/tmp/noteAppC-build/tools/noteAppC-index --fresh /tmp/bench-vault
```

## Building and looking at it

```sh
sh tools/check.sh              # both configurations, every suite; --bench adds the bench
/tmp/noteAppC-build/apps/noteAppC/noteAppC <vault-or-file>
```

Run builds and tests in a memory-capped scope (a runaway test once took the
whole session down with it): `systemd-run --user --scope -q -p MemoryMax=8G
-p MemorySwapMax=0 <command>`. `check.sh` does this itself.

Under AddressSanitizer and UBSan, which found a use-after-free in a test and
a malloc/delete mismatch in MicroTeX on 2026-09-23 - worth a run after any
change to ownership or lifetimes. Wall-clock budgets fail under it, by design:

```sh
cmake -B /tmp/noteAppC-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build /tmp/noteAppC-asan
ASAN_OPTIONS=detect_leaks=0 QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/noteAppC-asan
```

Render the window offscreen, which is how the look gets reviewed without a
display:

```sh
/tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800
/tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800 --source
/tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 1400 860 --command graph:open
/tmp/noteAppC-build/tools/noteAppC_shot <vault> - out.png dark 1400 860 --command global-search:open --type "tag:#work"
```

`fixtures/demo-vault` is committed for exactly this: one note covering most of
the syntax, a second to link to and embed, and a `.obsidian` so the
vault-config path is exercised.

```sh
/tmp/noteAppC-build/tools/noteAppC_shot fixtures/demo-vault fixtures/demo-vault/Welcome.md out.png dark 1240 820
```

Use it rather than building a vault by hand, so the next person can retake the
same screenshot.

## Working with several agents in this tree

It works, with two rules. **Ownership is by file and nothing overlaps**: a
lane gets its own directories (a module in `libs/ext/`, its test file, one
clearly separated block in a shared file such as `Index.h`), and the lead
owns `libs/ui`, the root CMake, the merges and wiring. **Every lane works in
its own worktree under `/tmp`** (`git worktree add -b lane/<name>
/tmp/noteAppC-<name> main`), builds in `/tmp/noteAppC-<name>-build` with
`-j6` inside a 4 GB memory cap, commits there in small steps, and reports
its API and wiring instructions; the lead reviews, merges with `--no-ff`,
measures and wires. Seven lanes ran this way on 2026-09-23 (search, plugins,
graph, canvas, bases, editor blocks). Right-size the model per lane.
