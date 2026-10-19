# Performance

What noteAppC costs, how that was measured, every fix that changed it, and the
things that were tried and not adopted. Every number here was measured on
starbase-1 (Ryzen 9 9900X, 12 cores / 24 threads, RTX 5080, CachyOS) with the
default RelWithDebInfo build (`-O2 -g`) unless it says otherwise, and every one
can be reproduced with the commands in [Measuring](#measuring).

The standing requirement is Jernej's: noteAppC must be faster than Obsidian in
every way, being native C++, and stay fast on a machine with no GPU at all.

## The rules this work follows

1. **Measure before changing anything.** Every fix below was found with `perf`
   or the bench, not guessed. Twice a plausible guess was wrong: font
   enumeration was suspected at startup and cost 0.0 ms, and the MicroTeX gap
   was recorded as font coverage when it was a 1 pt raster.
2. **A budget nobody measured is not a budget.** The index budget was only
   ever extrapolated, and was out by a factor of eight.
3. **Guard with ratios, not wall-clock numbers.** Five times the text must
   cost about five times the work. A quadratic cannot hide inside a ratio
   however fast the machine is; it hides easily inside a generous timeout.
4. **Perceived speed never costs more than 10 ms of real speed.** Jernej's
   limit. A trade of real time for apparent speed has to be measured A/B.
5. **Nothing optional runs before the first paint or on the keystroke path.**
   Preloading is fine once the visually important parts are on screen.
6. **Numbers taken on a busy machine are not numbers.** See
   [Measurement pitfalls](#measurement-pitfalls).

## Where it stands

Median of several runs, after every fix below; a 2 000-note vault unless the row says 20 000. Rows dated
2026-10-16 are the best of three interleaved runs at `d9fcfae` (20 000 notes, the busy machine):

| what | median | budget |
|---|---|---|
| window on screen from process start | 50.5 ms | - |
| cold start, the process's first window (bench, includes `openVault`) | 36.3 ms (2026-10-16, perf2) | 40 ms |
| cold start to first painted frame, later in-process windows (bench) | 19.5 ms (2026-10-16) | - |
| pop-out window: command to first painted frame (bench, 2 000 notes, busy machine) | 10.7-11.8 ms (2026-10-18; three runs' medians) | 25 ms |
| move a tab to a new window (bench, same) | 3.1-3.4 ms (2026-10-18; includes the empty tab main gets in its place) | 5 ms |
| vault usable from process start | 204 ms | - |
| note open, 1 kB | 1.8 ms (2026-10-16) | 300 ms |
| note open, 50 kB | 7.9 ms (2026-10-16) | 300 ms |
| note open, 200 kB | 2.5 ms (2026-10-16; readable, styled ~26 ms later) | 300 ms |
| note open, 1 MB | 10.2 ms (2026-10-16; readable, styled ~140 ms later) | - |
| note open, notebook with 40 stored PNG outputs | 37.7 ms (2026-10-16) | - |
| Tags pane refresh, 20 000 notes, 4 505 distinct tags | 36 ms when shown; 0.00 ms while hidden (marked stale, rebuilt on show) | - |
| every note for a base (`allNotesForBases`), 20 000 notes, off the UI thread | 33 ms (2026-10-16, perf2; 65 ms before) | - |
| keystroke to relaid, painted block, 200 kB | 0.22 ms | 8 ms |
| keystroke through the whole window, 200 kB | 0.17 ms | 8 ms |
| keystroke, 1 MB | 0.55 ms (2026-10-16; 0.44 ms on a quiet machine) | 0.6 ms (this pass), 8 ms |
| source-mode table Tab, 1 MB | 2.0 ms (2026-10-16) | - |
| resize, text width kept / rewrapped, 1 MB | 0.24 / 1.36 ms (2026-10-16) | - |
| fold a callout, 1 MB | 0.29 ms (2026-10-16) | - |
| fold all / unfold all headings and list parents, 1 MB (10 958 folds: 5 479 headings, 5 479 list parents) | 1.81 / 1.52 ms (2026-10-19) | 50 ms |
| `foldState()` after fold all, 1 MB (10 958 folds; only asked on leaving the note) | 2.18 ms (2026-10-19) | 50 ms |
| keystroke on a heading with everything folded, 200 kB | 0.15 ms (2026-10-19) | 8 ms |
| keystroke in a code block, 100 / 1 000 / 5 000 lines | 1.10 / 1.10 / 1.16 ms | 8 ms |
| keystroke in a cell of a 50-row HTML table (2026-10-02, busy machine) | 1.33 ms | 8 ms |
| parse plus HTML structure pass, that note, gumbo cache cold (off the UI thread) | 0.24 ms | - |
| scroll frame, 1180x760 | 0.36 ms | - |
| scroll frame, one wheel notch, on screen (headless cage) | 0.4-0.65 ms | - |
| scroll frame, 3440x1440 (this machine's 240 Hz panel) | 0.88 ms, worst 2.1 ms | 4.17 ms |
| cold index, 20 000 notes | 0.95 s (2026-10-01, quiet; the 1.24 s of stage 14 was the busy machine); 1.04 s on 2026-10-16 with the tag-heavy note | 5 s |
| peak RSS, vault plus a 1 MB note | 228 MB (2026-10-16) | 250 MB (this pass) |

"Readable" is deliberate wording: above 128 kB a note is shown as plain
paragraphs at once and styled when its parse comes back from the thread pool.
See [the async parse](#8-a-big-note-is-shown-before-it-is-parsed).

## Against Obsidian

First head-to-head, 2026-10-16, starbase-1, Obsidian 1.14.4 (Electron 43, no
plugins, never signed in) against noteAppC at `c3e13ea` (Release). Both ran in
the same headless `cage` (1280x720, real RTX 5080 through GL, so Obsidian was
GPU-accelerated), in a throwaway `HOME`/XDG tree, pinned to CCD1 (cores
6-11,18-23) at `nice 19`, one app at a time, interleaved (noteAppC cold, noteAppC
warm, Obsidian cold, Obsidian warm, then the next round). Every cell is the
**median of 5**, and the machine was not quiet (load average 2 to 5, other
lanes building at idle priority), so read ratios, not milliseconds.

Vaults are `tools/gen_bench_vault.py` output (5 000 and 20 000 notes, seed
fixed). "Cold" is a fresh profile with no cache (Obsidian's IndexedDB metadata
cache, noteAppC's `index.db`); "warm" is the second launch on the same profile.
The note shown at start is `areas/Note 00000.md`.

| what | vault | noteAppC | Obsidian | how it was timed |
|---|---|---|---|---|
| window on screen, cold | 5 000 / 20 000 | 169 / 174 ms | 627 ms (5k) | process spawn to first `wl_surface` commit with a buffer, read from cage's server-side protocol log (same for both; includes cage start-up). Obsidian's own first-contentful-paint is 714 / 691 ms |
| note painted, cold | 5 000 | **207 ms** | 916 ms (867 ms by the log) | noteAppC: first commit after the title names the note (log). Obsidian: devtools, first frame after `.cm-content` holds the note text |
| note painted, cold | 20 000 | **217 ms** | 1 221 ms | as above |
| note painted, warm | 5 000 | **221 ms** | 1 019 ms (989 ms by the log) | as above |
| note painted, warm | 20 000 | **286 ms** | 1 897 ms | as above |
| cold index, whole vault | 5 000 | **243 ms** (240-248) | 5.7 s from spawn, 5.0 s from when its indexer starts | noteAppC: the "Indexed N note(s) in X ms" message in the status bar, which is the indexer's own clock (scan start to `finished` reaching the UI thread); one `grim` crop 2 s after launch, 5 cold launches. Obsidian: devtools, from `inProgressTaskCount` first above 0 to back at 0 with every note in `resolvedLinks` |
| cold index, whole vault | 20 000 | **1.07 s** (1.06-1.12) | 36.7 s from spawn, 35.8 s from its indexer's start | as above (crop 3 s after launch); 33x |
| metadata ready, warm | 5 000 / 20 000 | not separately visible (the note is up at 0.22 / 0.29 s) | 1.08 / 2.27 s | same Obsidian condition on a populated cache |
| RSS after the index settles | 5 000 / 20 000 | 185 / 214 MB | 1 165 / 1 541 MB | `VmRSS` summed over every process of the app (Obsidian: 8 of them) 3 s after settling |
| PSS, same moment | 5 000 / 20 000 | 121 / 152 MB | 747 / 1 130 MB | `smaps_rollup`, which does not count Obsidian's shared pages once per process |
| open a 1 kB note | 5 000 / 20 000 | 2.2 ms (bench) | 27 / 45 ms | noteAppC: `benchNoteOpen`, readable and painted, offscreen, no compositor. Obsidian: `leaf.openFile` to the second animation frame with the text in `.cm-content`, so one 60 Hz frame wait (about 8 ms on average) is inside it |
| open a 200 kB note | 5 000 / 20 000 | 3.5 ms (bench) | 48 / 74 ms | as above |
| open a 1 MB note | 5 000 / 20 000 | 13.8 ms (bench; styled about 140 ms later) | 130 / 158 ms | as above; Obsidian virtualises, so this is the first screenful |
| keystroke, 1 kB note | - | 0.38 ms (bench) | 4.2 ms | noteAppC: `benchKeystroke`, relaid and painted block. Obsidian: devtools `Input.dispatchKeyEvent` to the end of the frame that follows (keydown to a `MessageChannel` message posted from a `requestAnimationFrame`), main thread only, no raster or present. The 1 kB row is Obsidian's floor: the frame cadence alone |
| keystroke, 200 kB note | - | 0.38 ms (bench) | 8.8 ms | as above, cursor in the middle of the note |
| keystroke, 1 MB note | - | 0.73 ms (bench) | 26.2 ms | as above |
| peak RSS, vault plus a 1 MB note | 5 000 / 20 000 | 240 / 234 MB (bench `VmHWM`, note outside the vault) | 1 425 / 1 701 MB (RSS right after opening it, note inside the vault) | not the same thing twice: noteAppC's figure is a high-water mark of the bench process, Obsidian's a sample of the summed RSS |

noteAppC is ahead on every row: 4x to 7x on window and note on screen, 21x to
34x on the cold index (5 000 / 20 000 notes), 6x to 7x less memory (RSS), and
about 9x to 36x on open and keystroke. The "bench" rows are in-process figures and
not a like-for-like timing against the devtools ones (see the first caveat).

Caveats, in the order they matter:

- **Mixed methods in the open and keystroke rows.** noteAppC's come from
  `noteAppC-bench` (offscreen, no compositor, no vsync wait); Obsidian's from the
  Chromium event loop on a 60 Hz output and include up to one frame of wait. The
  1 kB row is the floor that shows how much of Obsidian's figure is cadence.
  Even after subtracting it, the 1 MB keystroke is about 22 ms against
  noteAppC's 0.7 ms. A headless app window has no input-to-photon path in either
  case.
- **The two index clocks do not start at the same place.** noteAppC's is the
  status-bar message (scan start to the `finished` signal arriving on the UI
  thread); Obsidian's is read from outside, and I give it both from process
  spawn and from the moment its indexer starts (0.7-1.0 s in). The bare
  `Indexer` in the bench measured 0.41 s (5 000) and 2.7 s (20 000) in the same
  hours, noticeably above the app's own 0.24 s and 1.07 s from the same binary;
  the difference was not chased, and this table uses the in-app message.
- **Obsidian's "ready" is `inProgressTaskCount == 0`.** There is no single
  public "metadata cache finished" event: `resolved` fires once per file.
- **A vault with thousands of distinct tags stalls noteAppC's window.** The first
  attempt put the 200 kB and 1 MB notes of the bench inside the vault. Those two
  notes carry about 4 500 distinct `#tagN` tags, and with that vault noteAppC
  showed the note only after **4.1 s** (5 000 notes) / **12.3 s** (20 000 notes)
  on a warm cache, and on a cold one the "Indexed" message came at 4.4 s (0.6-6.1)
  / 13.8 s with the status bar frozen before it. `perf` puts it in `openVault` ->
  `Index::open` -> `TagsPane::refresh` -> `computeCounts` -> `Index::notesWithTag`
  on the UI thread: one SQLite query per tag node. The table above uses vaults
  without those notes (a second, clean batch of 5 rounds); the 1 MB and 200 kB
  notes exist only in the first batch's vaults, so Obsidian's open, keystroke and
  peak-RSS rows come from there (its index there: 7.4 s / 39 s, warm note
  1.28 s / 2.18 s). Fixed since: see
  [fix 29](#29-the-tags-pane-ran-one-query-per-tag), and the bench vault now
  carries a tag-heavy note.
- **Defaults on both sides.** Obsidian ran in its default Live Preview on a vault
  whose only config was an empty `app.json`, with no network restriction (its
  update check, if it ran, is not in the timed path); noteAppC ran with defaults
  on an empty `XDG_CONFIG_HOME`.
- **Typing latency is not photon latency.** Neither side's figure includes
  compositing or scan-out.
- One Obsidian cold-index run at 20 000 notes took 98 s in the first batch (a
  stall, not reproduced; the clean batch gave 35.9-37.7 s).

Reproduce: the harness that produced this is not in the repository (it is a
few hundred lines of Python driving `cage`, `WAYLAND_DEBUG=server` and the
Chrome devtools protocol); every number above can be re-taken with those three
pieces and the vault generator.

## Measuring

```sh
cmake --build /tmp/noteAppC-build
/tmp/noteAppC-build/tools/bench/noteAppC-bench                     # the table, headless
/tmp/noteAppC-build/tools/bench/noteAppC-bench --json              # also JSON, for diffing
/tmp/noteAppC-build/tools/bench/noteAppC-bench --phase open        # one phase: memory, start, open, keystroke, save, relayout, scroll, pdfview
/tmp/noteAppC-build/tools/bench/noteAppC-bench --viewport 3440x1440
QT_SCALE_FACTOR=2 /tmp/noteAppC-build/tools/bench/noteAppC-bench --viewport 2560x1440
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    cage -- /tmp/noteAppC-build/tools/bench/noteAppC-bench --gpu    # raster vs OpenGL, headless
```

`--gpu` needs a real window and a real GPU context, but never a real monitor:
inside `cage` with the headless wlroots backend it gets a GPU renderer and one
virtual output, and nothing appears on any screen. Asking a desktop for a
particular screen does not work - Hyprland put a fullscreen test window on the
main panel regardless of `setScreen()`.

The bench generates its vault with `tools/gen_bench_vault.py` unless one is
given, never touches `~/.config/noteAppC`, and reports median, p95 and worst.

The `save` phase times `MainWindow::save()` (the path an autosave and Ctrl+S
share: `QSaveFile` commit with `fdatasync`, the recovery snapshot, the history
record) after one typed character, on a 200 kB and a 1 MB note in a small
vault of their own, 15 saves each. Run it with `TMPDIR` on a real disk: `/tmp`
is tmpfs on starbase-1 and `fdatasync` there costs nothing (0.14 / 0.48 ms
median, which says nothing about a disk). The bench also points
`XDG_DATA_HOME` at a private directory, so the snapshots stay out of the real
`~/.local/share/noteAppC/recovery`.

```sh
mkdir ~/.cache/bench-tmp && TMPDIR=~/.cache/bench-tmp \
    /tmp/noteAppC-build/tools/bench/noteAppC-bench --phase save
```

| save | median | p95 / worst | where |
|---|---|---|---|
| 200 kB note | 5.7 ms | 15.7 ms | starbase-1, NVMe (btrfs on LUKS), 2026-10-17, three runs |
| 1 MB note | 6.0 ms | 17.4 ms | as above |

The cost is the disk's flush, not the note's size: 200 kB and 1 MB are within
0.4 ms of each other. The worst sample is probably the first save of the note
(not checked sample by sample), which also keeps the pre-edit text as a
snapshot; later saves inside the recovery interval skip it. Nothing here runs on a keystroke, only 2 s after typing stops
(or 30 s into continuous typing), and a note is saved once however many tabs
show it.

To find where time goes, `perf` works on the real binaries:

```sh
QT_QPA_PLATFORM=offscreen perf record -g --call-graph=dwarf -F 299 \
    /tmp/noteAppC-build/tools/noteAppC_shot <vault> <note> out.png dark 900 600
perf report --stdio --no-children -g none --percent-limit 1.5
```

Qt 6 sends `qWarning` to the systemd journal, not stderr, whenever stderr is
not a terminal - piped, scripted, or run by an agent. Twice that made
instrumentation look like "this code never runs". Set
`QT_FORCE_STDERR_LOGGING=1`, or use `fprintf(stderr, ...)` for throwaway
probes. The screenshot tool and the bench now set it themselves.

The `relayout` phase times what is not a keystroke but still re-lays a note
out, on the 200 kB and 1 MB notes: a resize that keeps the text width (the
editor is wider than its readable line length, 1 100 to 1 000 px), one that
rewraps it (640 to 600 px), folding a callout in the middle, a code-block
renderer's result arriving for a block in the middle (deliver, the queued hop
and the paint), and Tab in a source-mode table. The `start` phase leads with
`Cold start, the process's first window` when run on its own: the median row
below it is in-process windows after the first, which skip what a process does
once (fonts, plugins, the style sheet), so that row is what a launch pays. The
generated vault carries `tag-heavy.md` (3 000 flat and 1 500 nested tags) so
the Tags pane has real work; `firstNoteIn` skips it, so the "vault note" rows
stay about an ordinary note.

The `start` phase also times pop-out windows (`benchPopout`): "Pop-out: open to
first painted frame" runs `workspace:open-in-new-window` through one painted
frame of the new window (a second window and one editor); "Move tab to new
window" runs `workspace:move-to-new-window` alone. Off costs nothing
measurable: the `popout-windows` feature is one `bool` and one
`Settings::Feature` row, and `PopoutWindowsOffBuildsNothingAndDocksEverything`
pins that no window object exists then.

On a shared machine compare builds **interleaved**: base and candidate
alternate run for run (three each, best median per row), each run in a fresh
XDG tree, pinned with `taskset -c 6-11,18-23 chrt --idle 0`. Back-to-back
batches drifted by more than the changes being measured.

## Note sharing: two text CRDT backends

Jernej asked to try both a Rust (yrs) and a native C++ text CRDT behind one
interface ([SHARE.md](SHARE.md)) and compare them. One suite runs over both;
`test_share_crdt --gtest_filter='*Megabyte*:*SizesAndMemory*'` prints these
numbers (starbase-1, RelWithDebInfo, quiet machine, load average 0.9; three runs
agreed to 2%). Every edit is one character into a note of generated markdown.

| what | yrs | Native |
|---|---|---|
| type at the end, 100 KB note (10 000 edits) | 0.079 ms | 0.0001 ms |
| type at the end, 1 MB note | 0.75 ms (1.3 ms when first recorded in SHARE.md) | 0.0001 ms |
| edit at a random position, 1 MB note (2 000 edits) | 0.102 ms | 0.0026 ms |
| take in one remote edit at a random position, 1 MB note fragmented by 2 100 edits (500) | 0.237 ms | 0.0166 ms |
| bytes of the update for one keystroke | 14 | 18 |
| `encodeState()` of the 1 MB note | 1 000 016 B | 1 000 015 B |
| `diffSince()` after 100 scattered edits on a 1 MB note | 1 107 B | 1 404 B |
| heap in use after building the 1 MB note from 10 000 appends of 100 characters (`mallinfo2`) | 1 026 KB | 2 052 KB |

Reading it: both stay far inside the 8 ms keystroke budget, so the choice is
not about speed. Native is 14x to 1 000x faster per edit because typing in one
place grows one run (yrs walks the block that holds the position, hence the
cost growing with the note) and a position jump walks a linked list of runs; its
weak spot is the remote path, which walks every run (its cost grows
with the number of runs by construction, not the note). It costs twice the memory because it holds
UTF-16 where yrs holds UTF-8, and its updates are a few bytes larger (it
carries a Lamport timestamp per run, and the whole delete set per diff). Both
count the same 1 MB for the state. Native has no garbage collection of
tombstones; neither is tested for a history of millions of edits.

## Guards in the test suite

| test | asserts |
|---|---|
| `MarkdownEditorTest.KeystrokeStaysInsideTheBudget` | under 8 ms per keystroke on a 200 kB note |
| `FindTest.SteppingThroughALongNoteStaysInsideTheKeystrokeBudget` | find-next stays inside the same 8 ms |
| `MarkdownEditorTest.OpeningALargeNoteIsFast` | a 200 kB note opens in under 300 ms |
| `MarkdownEditorTest.OpeningAMegabyteNoteIsImmediate` | time to readable grows slower than the text |
| `ParserPerfTest.ParsingStaysLinearInFileSize` | 5x the text costs under 10x the parse |
| `ListPerfTest.TypingInALongListDoesNotDependOnItsLength` | 5 000 items type and reveal inside 8 ms, and within 4x of 100 |
| `CodeBlockPerfTest.TypingInALongCodeBlockStaysInBudget` | 5 000 lines inside 8 ms, and within 4x of 100, after the parse has landed |
| `EpubViewTest.ChapterRenderRunsOffThreadAndArrivesAsynchronously` | a cache miss never blocks; the chapter lands within 2 s of being asked for |
| `EpubViewTest.AStaleRenderFromBeforeAResizeIsDiscardedNotCached` | a render started before a resize/font/theme change never overwrites the fresh one |
| `EpubViewTest.DeletingTheViewWhileAWorkerRenderIsInFlightIsSafe` | deleting the view mid-render is safe (also run clean under ASAN+UBSan) |
| `EpubViewTest.TocIsNotBuiltAtOpenEvenForAHugeBook` | `open()` builds zero TOC rows, however large the book's TOC |
| `EpubViewTest.TocExpandsOneNodeAtATimeAndOnlyOnce` | a node's children exist only after it is expanded, and only once |
| `EpubViewTest.FontFaceIsRegisteredSynchronouslyBeforeTheWorkerIsDispatched` | `@font-face` registration has already happened before the worker is dispatched |
| `EpubViewTest.RemoteFontFaceIsNeverFetched` | a remote `@font-face` source is never attempted |
| `EpubViewTest.PagedModeNeverScrollsPastTheDocumentBounds` | paged navigation clamps to `[0, documentHeight - pageHeight]` |
| `CanvasPerfTest.ColdFirstPaintCostTracksVisibleNodesNotTotalNodes` | a cold first paint of 1 000 nodes costs under 6x one of 60 that all fit the viewport (about 1x; a broken cull is about 16x) |
| `CanvasPerfTest.SnapCostPerMouseMoveDoesNotGrowWithTheCanvas` | a snapped drag's cost per mouse move with 20 000 cards is under 20x that with 100 (about 1.3x; rebuilding or scanning the snap lines every move is about 500x) |
| `CanvasPerfTest.ScrollingASelectedCardCostsAGrabNotARelayout` | a wheel tick plus repaint of a scrolled card is under 10 ms (about 0.35 ms) and the hidden scroll editor is built once, not per tick (rebuilding it per tick is caught by the build count) |
| `MarkdownEditorTest.AResizeRewrapsOnlyWhenTheTextWidthChanges` | a resize that keeps the text width keeps every layout; one that changes it rewraps to the same heights a fresh layout gives |
| `DisplayMapTest.BinarySearchesAgreeWithALinearWalk` | `toDisplay` and `isHidden` agree with a linear walk on 3 000 random maps |
| `TableAutoTest.KeysInsideTheParseDebounceDoWhatTheyDoAfterIt` | source-mode Tab and Enter typed faster than the parse do what they do with a parse between keys |
| `IpynbTest.StoredOutputsAreWrittenOnlyWhenLookedUpAndARunWins` | reading a notebook writes no output-cache entry; a run or a clear before the lookup wins |
| `IndexTest.UnlinkedMentionsReadAFrontMatterLongerThanTheFirstRead` | a 40 kB front matter still yields its aliases |
| `IndexTest.TagCountsAgreeWithNotesWithTag` | the one-query tag counts equal `notesWithTag().size()` for every tag and parent |
| `CrdtDocTest.TypingAtTheEndOfAMegabyteStaysUnderTheKeystrokeBudget` | one character at the end of a 1 MB shared note costs under 3 ms and under 25x the cost at 100 KB, for each CRDT backend |
| `CrdtDocTest.MalformedUpdatesWithAValidHeaderAreCheap` | 20 000 bad Native updates are rejected in under 2 s: a size claim is never believed before the bytes are counted |
| `CrdtDocTest.AHostileDependencyChainIsLinear` | 100 000 Native runs each waiting on the next are placed in under 3 s (one pass per run would be quadratic) |

Every wall-clock guard above keeps the best of three runs (`tests/Perf.h`,
`perf::bestOf`), because a loaded machine - a parallel build, a lane-check next
door - only ever adds time, so the fastest run is the cost of the code. The
budgets themselves are unchanged, so a real regression still fails all three.
Under ASan or TSan the guards skip (`SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER`):
the clock would measure the instrumentation.

The last of the editor rows used to live in the editor tests. When the parse moved off the UI
thread it started measuring the layout path instead, and would have gone on
passing while the quadratic it existed for came back. It now measures
`Parser::parseAll` directly, which is where the bug was.

## The fixes

In the order they landed.

### 1. Keystroke: 375 ms to 0.09 ms on a 200 kB note

`7528eee`. The first working engine took 375 ms per keystroke against an 8 ms
budget. Four changes, each measured:

| change | per keystroke |
|---|---|
| lay out only the blocks near the viewport, estimate the rest | 375 -> 212 ms |
| apply a keystroke incrementally: shift ranges, relayout one block, parse later | 212 -> 68 ms |
| parse on the thread pool with a generation counter | no change alone; keeps the budget honest on big notes |
| memoise line height per block kind | 68 -> 0.09 ms |

The last one was the real cost all along: a `QFont` and its `QFontMetricsF`
built for each of 7 595 blocks, twice per layout pass.

### 2. Cold index: quadratic, 39.8 s to 3.6 s for 20 000 notes

`11f1a95`. The full-text table was deleted by a non-indexed column, so every
note scanned the whole table.

| notes | before | after |
|---|---|---|
| 5 000 | 3.49 s | 0.98 s |
| 10 000 | 14.68 s | 1.57 s |
| 20 000 | 39.80 s | 3.57 s |

### 3. Backlinks read every link in the vault

`036dc7f`. `Index::backlinks()` selected every non-external link in the
database and called `resolve()` on each, so opening one note cost time
proportional to the whole vault.

The fix rests on one invariant: a target can only resolve to a note whose name
is the target's last path component. Obsidian resolves by name or by a path
ending in the name, and `cleanPath` keeps the last component through any `../`.
Links now carry `tname`, that component folded and without the extension,
indexed. Schema version 3.

| 1 kB note open | before | after |
|---|---|---|
| 500-note vault | 4.7 ms | 1.2 ms |
| 2 000-note vault | 14.6 ms | 1.4 ms |

It no longer depends on vault size at all.

### 4. Opening a note was quadratic, twice over

`8ce3cc3`. A 1 MB note took 822 ms to open. `perf` found two independent
O(n^2) bugs, and neither was md4qt.

**The comment pass.** `ObsidianSyntax::scan` ran the lazy `%%...%%` pattern
with `globalMatch(text, region.start)` once per unmasked region. On a file with
no comment in it, PCRE has to walk to the end of the text before it can say
there is no match, and a 1 MB note has 16 273 regions. It is now one pass over
the whole text, with each match assigned to its region by binary search.
Parse: **1 992 ms -> 142 ms**.

**The outgoing-links pane.** `OutgoingLinksPane::refresh` called `contextLine`
once per link, and each call re-read the note from disk, decoded it, and built
a whole `SourceMap` over it: O(links x file size). Under `perf` it was 30% of
the entire run. The note is now read once per refresh.

| note open | before | after |
|---|---|---|
| 50 kB | 9.4 ms | 6.9 ms |
| 200 kB | 53.4 ms | 25.7 ms |
| 1 MB | 822 ms | 146 ms |

After both, the profile is flat: md4qt's own paragraph parser is the largest
single item at 3.7%.

### 5. Show the window first, read the vault second

`0f8d041`. The whole vault load ran before `show()`, so nothing appeared until
everything was indexed and open. `openWhenShown()` now waits for the empty
shell's first paint and queues the open behind it.

A/B, same binary, seven runs each:

| order | window on screen | vault usable |
|---|---|---|
| load, then show | 194.5 ms | 195.4 ms |
| show, then load | **50.5 ms** | 204.2 ms |

The window appears 144 ms sooner at a real cost of **8.8 ms**, because the
shell is now painted twice. That is inside the 10 ms rule, but not by much.
Anything further in this direction has to make the first paint cheaper rather
than add another.

### 6. The settings page built itself twice before the window appeared

`3139a6f`. `SettingsPage::setContext` built all four pages of rows, and it ran
from the `MainWindow` constructor and again the moment a vault opened, in a
session that usually never opens settings. The rows are now built on the
page's first `showEvent`. The constructor went from **27.5 ms to 15.4 ms**.

Unlike fix 5, this costs no real time: the work is not moved, it is not done.

Where the remaining 15.4 ms goes: sidebar 7.4 ms, theme and style sheet
4.4 ms, main area 2.3 ms, everything else under 1 ms.

### 7. Five settings that cost nothing because they did nothing

`475a876`. Not a speed fix, listed because it touched the keystroke path: Tab
now inserts `editor.tabWidth` spaces instead of a hardcoded four. No
measurable change.

### 8. A big note is shown before it is parsed

`b02cc37`. After fix 4, the parse was still the whole cost of opening a note -
142 ms of a 1 MB note's 146 ms - all on the UI thread. Above 128 kB the text
now goes up at once as plain paragraphs and the parse runs on the thread pool;
the styled version replaces it when it arrives. Below 128 kB the parse is
faster than a frame, so it stays synchronous rather than flickering through an
unstyled state.

`plainParagraphs()` is what makes it work. Without it an unparsed note is one
block a megabyte long, and laying that out as a single `QTextLayout` costs more
than the parse did.

| time to a readable note | before | after |
|---|---|---|
| 200 kB | 25.7 ms | 2.2 ms |
| 1 MB | 146 ms | 11.3 ms |

The parse still costs what it cost. It is off the UI thread and no longer in
front of the reader.

Callers that must hold the parsed note (a canvas card's grab, a PDF export) call
`MarkdownEditor::finishParse()`: it waits on the pool and adopts the result
directly. They must not spin a nested `QEventLoop` for it - the canvas card is
rendered inside `paintEvent`, where a timer or deferred delete (closing the tab)
firing would free the view mid-paint.

### 9. The word count, recounted for every keystroke

`c77d7a5`. Every keystroke fired `textChanged`, and the shell answered by
recounting the words of the whole note on the UI thread - O(document) on the
keystroke path. The benchmark could not see it, because its keystroke row drove
a bare editor, not the window a person types into; a row that types through the
whole `MainWindow` was added for exactly this. Meanwhile `Parser::parseAll`
already counted the words, with the identical rule, on the worker thread, and
the editor threw the number away. The count now travels with the parse.

| keystroke through the window, 200 kB | before | after |
|---|---|---|
| median | 0.33 ms | 0.21 ms |

It also answered a question: the navigation work had installed an
application-wide event filter, so every event passes through
`MainWindow::eventFilter`. With the word count gone the window row sits at the
bare editor's number, so the filter costs nothing measurable.

### 10. A long code block was laid out whole on every keystroke

A fence was one block however long it was, so a keystroke in it laid out and
highlighted the entire listing. Release build (`-O3`), keystroke plus paint:

| lines | before | after |
|---|---|---|
| 100 | 2.7 ms | 1.10 ms |
| 1 000 | 26 ms | 1.10 ms |
| 5 000 | 158 ms | 1.16 ms |

The first measurement of 5 000 lines said 25.7 ms, and was wrong: above
128 kB the parse is async, and neither the bench nor the new test waited for
it, so both timed an unhighlighted paragraph. See the pitfalls below.

perf put about two thirds of the time in `QTextLine::layout_helper`, shaping
with HarfBuzz, and the highlighter under 4%. Taken apart on 5 000 lines: 35 ms
to highlight, 21 ms to lay out as plain text, but 112 ms to lay out with the
45 000 format ranges the highlighter produced - more than linear, since 1 000
lines took 14 ms.

Two changes:

- **The highlighter's formats are built once per Kate format.**
  `KSyntaxHighlighting::AbstractHighlighter::theme()` returns a copy, and
  `applyFormat` asked for it up to seven times per token. A `QTextCharFormat`
  per `Format::id()` is now cached: highlighting 5 000 lines 35 ms -> 21 ms.
- **A fence longer than 32 lines is split into 32-line blocks,** as a list
  became one block per item (`kCodePieceLines`). An edit relays out one piece,
  and a reparse relays out only the pieces near the viewport. The fence's
  chrome is drawn across its pieces - each paints its own band of one panel,
  the header, label and tabs go on the first, the caption on the last, line
  numbers carry on - and folding, copying and the hover label act on the
  whole fence. The caret anywhere in it reveals every piece.

Highlighting has to cross the pieces: a `/*` in one piece colours the next.
`CodeHighlighter::State` wraps KSyntaxHighlighting's line state, and each
piece records the state it ended in. A piece is highlighted from the end state
of the one before; when its own end state changes, the next piece is marked
stale and every later piece already laid out loses its layout, to be redone as
it comes into view. A state that converges again stops the redoing there, so
ordinary typing re-highlights one piece. States survive a reparse for pieces
whose text did not move, which matters: after typing near line 4 500 of 5 000,
the parse landing costs 3.25 ms with its paint, where re-highlighting the
listing from the top to reach the viewport would be about 20 ms.

Found on the way, and fixed separately in `43e96b5`: `applyEdit` shifted the
edited block's end twice, so until the reparse a block drew the start of the
next one as well - a flicker of duplicated text on every keystroke.

Not covered: a long listing inside a callout is still part of the callout's
single layout.

### 11. What a keystroke still did to the whole note

After fix 10, perf on a parsed 1 MB note (Release, UI thread only) put a third
of a keystroke in `SourceMap`'s constructor: the editor rescanned the whole
note for line starts on every edit, one `QString::at()` at a time. Then
`updateLayout()`'s own loops, four passes over every block, and
`shiftRanges`.

- `SourceMap::applyEdit` moves the line starts after the edit instead of
  rescanning, and the full scan uses the vectorised `indexOf`, which also
  speeds the parse. A randomised test holds it to a rebuild after 2 000 edits.
- `updateLayout()` settles what each block reveals and places it in one pass,
  finds the blocks near the viewport by binary search, and after a layout
  re-places only from the first block whose height changed.
- List items are found by binary search on the block, not by walking every
  item in the note, which layout did once per line and painting once per
  list block.

| keystroke, relaid and painted | before | after |
|---|---|---|
| 1 MB | 0.71 ms | 0.44 ms |
| 200 kB | 0.27 ms | 0.22 ms |
| 200 kB, through the window | 0.22 ms | 0.17 ms |

The list change needed list items to move with the text between a keystroke
and its reparse, and doing only that broke something the new guard caught:
the bullets of the items below an edit disappeared. A block below an edit
keeps its cached layout, and its `DisplayMap` was still in the old offsets;
the unshifted items had agreed with it by accident. So now the cached maps
shift with the text, and so does every decoration a layout or paint can read
- callouts, tables, embeds, maths, `#|` options, paragraphs - which closes the
latent hazard this page and the TODO had recorded: gutter numbers, code-row
wrapping and click positions on blocks below an edit were also read at stale
offsets for those 20 ms. That costs back about 0.05 ms at 1 MB, included
above.

### 12. A wheel notch repainted the whole viewport

`QAbstractScrollArea` repaints everything on any scroll. The editor now
overrides `scrollContentsBy`: when the step is smaller than the viewport and
the layout that runs with it moved nothing on screen, `viewport()->scroll()`
moves the painted pixels and only the exposed strip is painted, and
`paintEvent` finds the blocks in that strip by binary search instead of
walking to them. A page step, a layout that moved something visible, and the
OpenGL viewport all still repaint whole.

On screen in headless `cage`, 5 000-line note, 1180x760, median of 800 frames:

| | before | after |
|---|---|---|
| raster, 72 px steps (one wheel notch) | 1.00 ms | 0.40-0.65 ms |
| raster, page steps | 1.6 ms | 1.35-1.6 ms |
| OpenGL, 72 px steps | 1.7 ms | 1.7 ms |

The "after" runs came out bimodal, 0.40 or 0.65, across four runs; they were
taken while a parallel Android build was compiling, so contention is the
likely reason (see the pitfalls). Either way under two thirds of before.

`ScrollTest.AScrolledFrameMatchesAFreshOne` scrolls in odd-sized steps both
ways, then compares the backing store - what the blits actually left on
screen - against a full repaint. It fails when the blit is off by one pixel.

The on-screen bench gained the wheel-step rows, and lost a forced
`viewport()->update()` after each step that would have hidden the blit. Its
loop also reads the live scroll maximum now: once real heights shrank the
range, a step past the end changed nothing, painted nothing, and waited out
the 200 ms ceiling - a fifth of all frames in the first run.

### 13. Search read every matching body before confirming the first

`Index::search()` selected `rowid, path, body ... ORDER BY path`, and SQLite's
sorter copies every selected column of every match: a common word over 20 000
notes sorted 20 000 bodies so that 200 could be confirmed. The sorted query now
carries `rowid, path`, and each body is read by rowid only while fewer than
`limit` notes have matched. `noteAppC-bench`, 20 000 notes, a query matching
every note: 18.1 ms to 6.4 ms. A rare word: 0.05 ms either way. The search
runs on the thread pool, so neither ever reaches the UI thread.

### 14. The link suggester sized 60 000 rows on every theme pass

Each editor's `[[` suggester is a `QCompleter` over every link target in the
vault, about three names per note. Any application-wide style change - every
settings pass runs one, including the one when a vault opens - re-laid out its
hidden popup, and a `QListView` without uniform item sizes measures each row:
157 ms of a 20 000-note cold start, and again for every tab's editor. The rows
are one line each, so `setUniformItemSizes(true)` loses nothing. Cold start,
20 000 notes: 475 ms to 335 ms (the 475 included a double refresh, fix 15,
that made the suggester's list exist before the theme pass).

### 15. Two emitters of one refresh

Three panes listen to `Index::changed` themselves, and the window's own
handler refreshed all three again, so each save queried them twice. Once
`Index::open()` began to report a new vault (the graph and search panes need
it), the indexer's `finished()` calling the same handler made that twice per
vault open as well. And each pane rebuilt its list on every settings pass,
the tags pane's being a query over every tag in the vault. Now one refresh per
change, and a theme pass rebuilds a pane only when a colour it paints with
changed. Cold start, 20 000 notes: 335 ms to 285 ms; 303 ms before this
session's features, which added a local graph, a search pane and tabs to it.

### 16. A graph nobody can see

`GraphView` re-read every file and link in the vault on each `Index::changed`
(every save) and rebuilt the local graph on each note switch, whether or not
it was on screen. Hidden, it now only marks itself stale and catches up in
`showEvent()`. The sidebar's local graph is built at startup for this reason
alone: it costs nothing until its tab is opened.

### 17. A whole parse for a title

The backlinks pane's unlinked mentions re-read the note and ran the full
Markdown parser over it to learn its title and aliases, which only the file
name and the front matter hold. Measuring every feature off against on (the
Core plugins switches) showed it: a 1 MB note opened in 15 ms with the side
panes off and 156 ms with them on. Now only the front matter is parsed, and
the outgoing links pane no longer reads the note when it has no links. Note
open with the panes on, 20 000-note vault: 13 / 27 / 156 ms at 50 / 200 /
1 024 kB to 7.5 / 3.2 / 19 ms; unlinked mentions 0.33 to 0.16 ms.

### 18. The indexer wrote on the UI thread

A scan parsed across the pool, but each result came back to the UI thread as
a queued call that wrote the note to SQLite there. On a cold index those calls
arrived faster than the loop ran them, so it did nothing else: a 3 000-note
index kept a 5 ms timer waiting 506 ms. And `openVault` asked the cache about
every file on the UI thread before the scan began. Now the whole scan - walk,
`isCurrent`, attachments, removals, writes - runs on a thread of its own with
its own connection (`Index::Writer`); the UI thread takes only each 256-note
chunk's names and progress, and one `changed()` at the end. The writer's
statements are prepared once, not per row, which halved the write cost.
20 000 notes: cold start 277 ms to 65 ms, cold index 1.97 s to 0.98 s, peak
memory 376 MB to 196 MB; the 3 000-note timer's worst wait 6 ms. A scan that
stop() cancels drops what it had queued: its names never reach the next
vault's table (`IndexerThreadTest`).

### 19. Every switch restyled the whole window

A settings switch runs `applySettings`, and it restyled the application
(`setStyleSheet` over every widget) and gave every open editor its theme again,
which relays out every block: 21.6 ms with a 1 MB note open, for a switch that
changes neither. `Theme` now compares, and the restyle waits for the theme
itself to change; a pane built by a switch takes the theme when it is built,
and the settings page re-themes the rows it rebuilds (they had kept the
default colours until the next restyle). Settings pass, 1 MB note open: 21.6 ms
to 1.6 ms (bench row "Settings pass").

### 19b. A formatting colour relaid the whole note

A colour in Settings > Appearance > Formatting colours changes only the
formats a block's `QTextLayout` carries, but `MarkdownEditor::setTheme` zeroed
every block's height and estimated them all again (the 17-21 ms of item 19,
and the scroll position with it). `Theme::sameExcept` now says when the only
difference is one of the format-colour tokens, and `setTheme` then does what a
spelling refresh does: drop the layouts, keep every height, lay out the blocks
on screen. `QTextLayout::setFormats` clears a laid-out layout's lines (checked
by measuring `lineCount()` on either side of it), so those blocks cannot be
recoloured in place; the cost is the visible blocks only, 0.2 ms each. A size,
a font, the accent or the text colour is still the full path.

### 20. EPUB reader: chapter rendering off the UI thread, a lazy TOC, CSS resources, paged mode

Four gaps in the EPUB reader (`libs/ext/epub`), not from a profile - the
module is new enough that its own report named them directly. All four
numbers below are `tests/test_epub.cpp`'s own `Perf*` tests, printed via
`fprintf(stderr, ...)` (`QT_FORCE_STDERR_LOGGING=1 ctest -R epub`), not the
bench (there is no EPUB row in it yet).

**Chapter rendering.** Opening a book laid out and rasterised its first
chapter with litehtml on the UI thread, ~11 ms. `requestChapterRender()` now
does the archive/CSS/font work synchronously (cheap - string handling and a
QFontDatabase call, never litehtml) and hands the layout/raster itself to
`QtConcurrent::run`, delivered back through the same Lifeline +
generation-counter (here: `m_renderEpoch`) pattern `MarkdownEditor`'s embeds
already use (`PdfEmbedBlock.cpp`). A cache miss during paint shows the
already-known/guessed chapter height as a placeholder and repaints when the
worker delivers.

| | before | after |
|---|---|---|
| chapter render: time blocking the UI thread | ~11 ms | 0 ms |
| chapter render: total time to a cached, paintable chapter | ~11 ms (all on the UI thread) | ~10-11 ms (all off it) |

The work costs the same - litehtml did not get faster - only where it runs
changed. Page-turn time (paged mode, below) is a position update only
(0 ms); the page painting it triggers is the same off-thread render path.

**Lazy table of contents.** `buildToc()` used to run at `load()`, all levels
at once. It now runs only the first time the panel is shown, one level at a
time (a node with children gets a single placeholder row until it is
actually expanded - `onTocExpanded()`). Measured on a synthetic 50 000-entry
TOC (50 volumes x 1 000 chapters each - `makeEpubWithHugeToc`):

| | cost |
|---|---|
| `open()` | 0 (deferred entirely) |
| first show (50 top-level rows) | 346 us |
| one volume expanded (1 000 rows) | 429 us |
| the other 49 volumes (49 000 rows - what an eager build at `open()` used to pay) | 20.1 ms |

At the 2 000-entry scale the committed regression test uses (20 x 100, cheap
enough to run on every `ctest`), all three numbers round to 0 ms - see
`PerfTocBuildOnAHugeToc`.

**CSS `url()` resources.** A chapter's stylesheets (external `<link>` sheets
and inline `<style>` blocks) never had their `url()`s resolved into the
archive at all - a `background-image` drew nothing, and no `@font-face`
source was ever loaded. Both are now extracted the same way an `<img src>`
already was (`rewriteCssUrls`), and every `@font-face` source is registered
into the application font database once per archive path
(`registerChapterFonts`). Not a speed fix, but a thread-safety one: this
registration is why the calling thread still does the archive/CSS work
before dispatch in the chapter-rendering fix above -
`QFontDatabase::addApplicationFontFromData` is not safe off the Qt GUI
thread, the exact rule that already keeps MathRenderer/MicroTeX font loading
on `Publisher`'s calling thread rather than its worker pool
(`PageBody.h`/`Publisher.cpp`, "publish, PDF viewer, plugins and sync
merged"). `FontFaceIsRegisteredSynchronouslyBeforeTheWorkerIsDispatched`
checks the registration count is already 1 the instant
`requestChapterRender()` returns, before the worker has run at all.

**Paged mode.** New, not a fix: one or two page-height slices of the same
continuous document-space litehtml already lays out (`m_vscroll`'s value,
snapped to multiples of the viewport height), rather than a second layout
path - so the render cache, hit-testing and highlight painting needed no
changes to work in both modes. Page-turn cost above; column count picked
from viewport width (`kTwoColumnMinWidth`, 900 px). See "Guards in the test
suite" above for the regression tests this fix added.

### 21. Stage 14: a note's blocks were moved every time the list grew

`rebuildBlocks` pushed each block straight into a `std::vector<BlockView>`
with no reserve. A `BlockView` is 1.6 kB (layout, cells, callout style, code
state), so every regrowth moved every block built so far: under `perf` the
move constructor, the destructor and `_M_realloc_append` were a quarter of
the open phase. The spans are now cut into a vector of small parts first,
and the views are built once in a vector sized for them. The same pass read
`view.span` after `std::move(view)`, which worked only because the span is
trivially copyable; the part is now read before it is stored.

Two side fixes from the same run. `OutgoingLinksPane` passed an empty path
to `Vault::read` when a note had no links, and Qt warned about it on every
note open (83 warnings a run). `PdfView`, `PdfPagesArea`, `EpubView` and
`SlidesView` held a default-constructed `Theme` until `setTheme` was called,
whose zero font sizes produced 4 602 `QFont::setPixelSize` warnings a
pdfview run; they now start from `Theme::builtin(Dark)`, as the editor and
the window already did.

`noteAppC-bench --phase <memory|start|open|keystroke|scroll|pdfview>` runs
one phase, so a profile covers only what it is about. The start phase now
also has a `Cold index` row: a full scan into an emptied cache, the time the
indexer reports, on the generated vault only (a vault given on the command
line keeps its cache).

| note open | before | after |
|---|---|---|
| 1 kB | 2.3 ms | 3.2 ms |
| 50 kB | 11.3 ms | 12.0 ms |
| 200 kB | 6.3 ms | 3.9 ms |
| 1 MB | 38.6 ms | 11.9 ms |
| peak RSS | 262 MB | 233 MB |

The 1 kB and 50 kB rows are within run-to-run noise with another build
running on the machine. Of the 50 kB open, 7.1 ms is md4qt's parse, run
synchronously below the 128 kB threshold; the blocks, layout and panes are
the other 2 ms, and the first frame the rest.

### 22. Tab in a source-mode table reparsed the whole note

`handleSourceTableKey` rewrote the table and then called `reparseNow()` so the
next key would find the table again: the whole note, synchronously, on every
Tab or Enter. The table is now found in the decorations as `shiftRanges` left
them, its rows are re-read from its own lines, the caret is placed from the
rewritten lines, and the reparse goes back to the normal 20 ms debounce. Only
when the shifted decorations no longer describe the table (no table found, or
the header's cell count changed) does it still reparse first.

| Tab in a source-mode table | before | after |
|---|---|---|
| 1 MB note | 340 ms | 2.07 ms |

### 23. A notebook's stored outputs were all written at open

Reading an `.ipynb` decoded every stored output and wrote each into the output
cache before the note was shown: with 40 plots of about 1.2 MB, 230 ms of a
358 ms open. `seedNotebookCache` now keeps a closure per key; the first lookup
of that key (only blocks that are laid out look themselves up) decodes and
writes it, and a run or a clear before then drops it. See
[NOTEBOOKS.md](NOTEBOOKS.md#jupyter-notebooks-ipynb).

| note open | before | after |
|---|---|---|
| notebook, 40 PNG outputs | 351 ms | 35.4 ms |

### 24. Unlinked mentions read every candidate whole for its front matter

`unlinkedMentions` read each candidate note in full to find its aliases. It
now reads the first 16 kB and goes back for the rest only when the note opens
a front matter that has not closed by then. `Vault::decode` no longer runs a
CRLF replace over text that has no `\r`. The bench vault's notes are about
1 kB, so its rows did not move (unlinked mentions 0.165 to 0.163 ms, vault
note open 4.05 to 3.85 ms); the read is bounded for long notes now.

### 25. A resize, a fold or a link turning resolved threw away every layout

`invalidateLayouts()` reset every block, and it was the answer to anything
that was not an edit. Now:

- a resize relays out only when `contentWidth()` changed - the left margin is
  added at paint time, so a window wider than the readable line length keeps
  every layout;
- a fold invalidates that one block;
- a link-status change invalidates only blocks with a link to a target that
  flipped.

Narrowing the resize exposed a bug the full invalidation had hidden:
`updateLayout()` lays out the window around the scroll position from estimated
heights, and when real heights then moved other blocks into view they stayed
unlaid, and undrawn, until something invalidated everything. The fuzz test
caught it as pixel differences. `updateLayout()` now repeats until nothing
changes (at most 8 passes), and the passes after the first cover only the
viewport, which is what a rewrapping resize pays for.

| relayout | before | after |
|---|---|---|
| resize, text width kept, 200 kB / 1 MB | 0.83 / 1.29 ms | 0.19 / 0.25 ms |
| resize, text rewrapped, 200 kB / 1 MB | 0.79 / 1.26 ms | 0.86 / 1.36 ms |
| fold a callout, 200 kB / 1 MB | 0.88 / 1.41 ms | 0.23 / 0.30 ms |
| media arrival, 200 kB / 1 MB | already one block | unchanged |

The rewrap rows are the fixed point's price, inside the 15% this pass allowed
(+11.4% / +9.4% in the final interleaved run against the pre-pass build).

### 26. DisplayMap walked its runs from the start

`DisplayMap::toDisplay` and `isHidden` walked every run up to the offset. The
runs are in source order with non-decreasing ends, so both are a
`lower_bound` / `upper_bound` now. Small at today's sizes; it is on the path
of every caret move in a note with hidden syntax.

### 27. Every window re-applied the application style sheet

`MainWindow` set the palette and the style sheet in `applyTheme()`, after its
widgets existed, so they were polished twice; and setting an unchanged sheet
still repolishes every widget in the application. The theme is now applied to
the application before any widget is built, and `applyTheme()` sets the
palette and the sheet only when they differ.

| what | before | after |
|---|---|---|
| cold start, in-process windows (median) | 32.0 ms | 19.4 ms |
| cold start, the process's first window | 40.4 ms | 40.7 ms |
| settings pass, 1 MB note open | 1.59 ms | 0.70 ms |

The median row fell because later windows no longer repolish the earlier
ones; a real launch is the first-window row, which did not move. With fix 28
on top, the final interleaved run put it at 42.6 to 40.3 ms.

### 28. Every editor read the syntax definitions

Each `MarkdownEditor` owns a `CodeHighlighter`, whose constructor built a
`KSyntaxHighlighting::Repository` - reading every definition's metadata, 2.3 ms
- whether or not the note had a line of code. The repository is now made on
the first `knows()` or `highlight()`. A canvas builds an editor per visible
node:

| canvas cold first paint | before | after |
|---|---|---|
| 60 nodes, all on screen | 188-210 ms | 63-88 ms |

That made the canvas cold-paint guard flaky. Its "60 nodes" were laid out 40
columns wide, so only 16 of them were on screen and the honest ratio against
1 000 nodes was about 4x, under a 6x limit; the 2.3 ms per editor had been
padding both sides. The small canvas is now 8 columns wide, so the ratio is
about 1x, and a broken cull (`isNodeVisible()` always true) was checked to
fail it at about 16x.

### 29. The Tags pane ran one query per tag

`TagsPane::refresh()` asked `Index::notesWithTag()` for every node of the tag
tree, each a full scan of the `tags` table (its `LIKE` cannot use the name
index), on the UI thread, at open and on every `Index::changed`. Found by the
[Obsidian comparison](#against-obsidian). `Index::tagCounts()` reads the
(note, tag) pairs once and adds each note once to its tag and every
`/`-parent.

| Tags pane refresh, 20 000 notes, 4 505 distinct tags | before | after |
|---|---|---|
| warm index | 9.5 s | 38 ms |

### 30. The Tags pane rebuilt while nobody could see it

`TagsPane::refresh()` is 36 ms at 4 505 tags (half query, half
`QTreeWidget`) and ran on every `Index::changed`, shown or not. A hidden pane
now sets `m_stale` and returns; `showEvent` refreshes a stale one. The two
window tests that read the pane now show the window and click its tab first,
and the first also asserts the hidden list is still empty. The bench has a row
for each case.

| Tags pane refresh, 20 000 notes, 4 505 distinct tags | before | after |
|---|---|---|
| index change while the pane is hidden | 37 ms | 0.00 ms |
| the pane being shown | 37 ms | 36 ms (paid once, on show) |

### 31. `allNotesForBases` read 220 000 rows one after the other

Every base block, view switch and publish calls it, always on a worker
(`BaseView::openCurrentView`, the editor's base runner, `Publisher`), never
on the UI thread, so the cost was latency before a base appeared: files 5 ms,
props 15, tags 14, links 14, plus a QVariant per cell. Two changes. Each query
is `setForwardOnly(true)`, which drops Qt's row cache (65 to 61 ms). The
three child tables are then read on their own threads and connections (WAL
lets readers run side by side) into lists indexed like the notes and moved in,
so the wall time is the slowest one. `std::async`, not the global pool, because
the caller may itself be on that pool. `GROUP_CONCAT` per note was tried and
was slower (73 ms: the sort for `GROUP BY` and the splitting cost more than
the rows saved). Same results; the bases, publish and index suites pass.

| every note for a base, 20 000 notes | before | after |
|---|---|---|
| bench row (includes freeing the result) | 65 ms | 33 ms |

### 32. The first window paid libgit2's certificate load

`--phase start` on the first window splits as constructor 19.5 ms, `openVault`
15 ms, show and paint 6 ms; later windows total 19 ms. A sampled profile of the
first window put 5 ms of `openVault` in `NoteHistory`'s constructor, which
runs `git_libgit2_init()` once per process, and that loads the system's CA
bundle through OpenSSL. `NoteHistory::warmUp()` runs the same function-local
static on the history pool from the window's constructor (when the feature is
on), so it overlaps the 19 ms of widget construction; whoever needs libgit2
first waits on the static. Nothing was shut down or reordered, so the crash
the comment above it records (shutdown racing init) is not reintroduced.

Also profiled and left: font database population and the first font load are
about 5 ms but start at the first widget, with no earlier work to overlap
(warming it on a worker from the constructor's first line changed nothing);
`Indexer::rewatch` was 6.7 ms of directory walking on the UI thread in every
window. The scan's own walk now lists the directories too and hands them to
`Indexer::watch` with each one's mtime as read; a directory whose mtime moved
before its watch started triggers a rescan, so the gap is closed. Only the
difference is (re)watched, and folders made later are now watched too (they
were not before). First window 36.3 -> 28 ms (best of 3, 20 000 notes).
A folder inotify will not watch (past `fs.inotify.max_user_watches`, or no
permission) is no longer dropped silently: the status bar says so once and
the indexer rescans every 30 s while any folder lacks a watch. With every
folder watched the poll timer never starts.

| cold start, the process's first window (bench), best of 3 | before | after |
|---|---|---|
| 20 000 notes | 41.5 ms | 36.3 ms |

### The graph view at 20 000 notes

A step of the force layout is O(n log n) and a frame is linear in nodes and
edges, and both ran on the UI thread: at 20 000 notes and 40 000 links a step
was 22-28 ms and a frame 18 ms, so a settling graph held the window for ~65 ms a
tick and every hover repainted for ~18 ms. Opening the graph read the index
(`Index::graphData()`, 100 ms with its 40 000 link resolutions) and built the
node lists on the UI thread too, and every note save did it again, scattering
every node (`m_layout.reset()`) as it went.

Now: stepping and painting are one `QtConcurrent` job at a time, painted into a
`QImage`; the UI thread draws that image (and, between jobs, maps it from the
camera it was painted with to the current one, so pan and zoom are instant).
The index read, link resolution and node/edge building run on a worker over a
snapshot (`Index::graphDataIn`, same pattern as `searchIn`); the rebuild keeps
every surviving node where it was. Threading contract: GraphView.h.

| 20 000 notes / 40 000 links, best of three | before | after |
|---|---|---|
| layout step (unchanged, on a worker now) | 28.5 ms | 22 ms (run-to-run noise) |
| frame (on a worker now) | 17.9 ms | 8.5 ms |
| UI thread per paint | 17.9 ms | 0.2 ms |
| `setIndex()` blocks the UI | 123 ms | 0.0 ms |
| longest UI stall while the graph loads | 123 ms | 6.8 ms |
| graph ready after `setIndex()` | 123 ms | 83 ms |
| 5 000 nodes, step / frame (the old guard) | 5.5 / 9.5 ms | 4.4 / 2.8 ms |

`GraphPerformanceTest.TwentyThousandNodesReportCosts` prints the first four;
`BigVaultOpenCosts` (set `NOTEAPPC_GRAPH_BENCH_NOTES=20000`) prints the rest.
Left: a save while the graph is visible waits for the job in flight before it
rebuilds - one job, ~35 ms at 20 000 notes, but up to 250 ms with Animate off,
which runs a settling pass per job.

## Tried and not adopted

### OpenGL viewport

`0a5fefc`. `MarkdownEditor::setGPUacceleration` swaps the viewport for a
`QOpenGLWidget`, which is the supported way to put widget painting on the GPU
without QML. It is a setting, **off by default**, because it measured slower.

On screen, vsync off, 5 000-line note, three runs:

| viewport | whole scroll sweep | median frame | worst frame |
|---|---|---|---|
| raster | ~420 ms | 1.05-1.72 ms | 11.6-12.0 ms |
| OpenGL | ~467 ms | 2.16-2.20 ms | 5.1-5.3 ms |

About 11% slower overall - the GL paint engine's glyph cache is the usual
reason for text - with a steadier tail.

Re-measured headless in `cage`, where both viewports get the same virtual
output and near-equal frame counts (420 and 444), so frame by frame is fair:

| viewport | median frame | worst frame |
|---|---|---|
| raster | 0.70-0.98 ms | 5.3-5.6 ms |
| OpenGL | 1.57-1.68 ms | 4.3-4.8 ms |

The same verdict: slower on average, slightly steadier in the tail. The per-frame medians are not
comparable on their own, because the GL viewport scrolled in larger steps: 216
frames against 400. The whole sweep is the fair number.

### Vulkan

Qt 6.11 on this machine has Vulkan and `QRhiWidget`, so a Vulkan path is
buildable. It is not a switch, though: there is no Vulkan paint engine for
`QPainter`. Vulkan in a widget application means `QRhiWidget` plus a renderer
of our own - a glyph atlas built from `QRawFont::alphaMapForGlyph`, glyph
positions taken from the `QGlyphRun`s the existing `QTextLayout`s already
produce, one instanced-quad pipeline, and shaders compiled with Qt's `qsb` so
the same source serves Vulkan, OpenGL and Metal. Every panel, caret, selection,
image and callout the editor paints today would have to be redrawn that way.

Whether that is worth building depends on where raster runs out, which is
pixels. Full-page scroll of a 5 000-line note:

| viewport | physical | median | worst |
|---|---|---|---|
| 1180x760 @1x | 0.9 Mpx | 0.36 ms | 0.67 ms |
| 3440x1440 @1x | 5.0 Mpx | 0.88 ms | 2.1 ms |
| 3840x2160 @1x | 8.3 Mpx | 2.26 ms | 4.13 ms |
| 1920x1080 @2x | 8.3 Mpx | 2.24 ms | 5.90 ms |
| 2560x1440 @2x | 14.7 Mpx | 6.89 ms | 8.61 ms |
| 3840x2160 @2x | 33 Mpx | 18.2 ms | 21.7 ms |

This machine's main panel is 3440x1440 at 240 Hz, a 4.17 ms budget, and raster
fits it with room. The editor's column is capped at a readable width, so most
of a wide window is flat background, which is cheap. Raster stops keeping up
at a 5K panel at 2x (6.9 ms, over a 144 Hz budget) and falls below 60 Hz at 6K
and beyond.

So the order is:

1. **Scroll by blitting.** Done, fix 12: a wheel notch now moves the pixels
   already painted and repaints only the strip that came in.
2. **Measure the Wayland buffer transfer.** Measured 2026-09-23, as a proxy:
   the compositor's own CPU time while the on-screen bench scrolls, taken
   with `perf stat -i` on `cage` (its threads only, not the bench inside it),
   minus a run that opens the window and scrolls nothing. User time only,
   two runs each, 1180x760:

   | viewport | compositor CPU per frame | the app's own frame |
   |---|---|---|
   | raster (shared-memory buffer, uploaded) | 0.28-0.31 ms | 0.34-0.65 ms (wheel) |
   | OpenGL (the buffer stays on the GPU) | 0.065 ms | 1.56 ms (wheel) |

   So the copy is real - a quarter of a millisecond of someone's CPU per
   frame at this size, roughly 1.4 ms at 3440x1440 if it scales with the
   pixels, which is an estimate, not a measurement - but raster still costs
   less in all, app and compositor together, than OpenGL does here. And it is
   cage's wlroots GLES2 renderer, not Hyprland; the number to trust would be
   Hyprland's own, which cannot be taken without a window on a real monitor.
   Reproduce with `noteAppC-bench --gpu --only none|raster|opengl` under
   `perf stat -i -e task-clock -- cage -- ...`.
3. **Then a Vulkan renderer**, only if HiDPI panels matter or step 2 says the
   copy does. It would sit behind the same `editor.gpuAcceleration` setting.

## Measurement pitfalls

Each of these produced a wrong number at least once in this project.

- **Contention.** The subagent that built the bench measured a 1 MB keystroke
  at 13.7 ms, over budget, while two other builds were running. On a quiet
  machine it was 0.78 ms.
- **Rendering a GL widget into a `QImage`** reads its framebuffer back over the
  bus and measures the readback, not the drawing. The GPU comparison had to be
  on screen, with vsync off, or every sample comes back as one refresh.
- **A frame captured at logical size.** `paintedFrame()` built its image at the
  widget's logical size, so a 2x run painted a quarter of the pixels and called
  it 2x. It surfaced because the 2x and 1x numbers came back identical. Fixed in
  `25ea55a`.
- **Unequal step counts.** Two viewports scrolling the same document in
  different step sizes cannot be compared frame for frame. Compare the sweep.
- **A guard that stops guarding.** Moving the parse off the UI thread changed
  what the editor's linearity test measured, without failing it. When the code
  path under a test changes, check the test still exercises the thing it was
  written for.
- **A tool that hangs looks like the code under test hanging.** A screenshot
  of a note that did not exist blocked forever on a modal error box nobody
  could dismiss offscreen, and it was first taken for a bug in the inline
  maths that had just landed. The tool now refuses a missing vault or note.
- **A benchmark that skips the real path.** The keystroke row drove a bare
  editor, so a whole-document word count the shell ran on every keystroke was
  invisible to it for as long as it existed.
- **Building while another agent edits a shared header.** A struct grew a
  field in `Decorations.h` in the middle of a build; some objects saw one
  layout and some the other, and a destructor crashed freeing a string that was
  not there. It looked exactly like a real bug. Rebuild everything that
  includes a header after it changes, and do not build over a live edit.
- **Exact-colour pixel counts on antialiased text.** Only a handful of a
  label's pixels land on its exact colour, and how many depends on Qt's glyph
  cache - a test passed alone and failed every time after another test had
  drawn text. Count pixels a channel clearly dominates instead.
- **Timing the placeholder.** Above 128 kB a note is plain paragraphs until
  its async parse lands. The code block row, and the first version of its
  test, never waited for it, and reported 25.7 ms for a 5 000-line listing
  whose real cost was 158 ms. The bench's keystroke rows now wait for the
  parse; so the 200 kB and 1 MB rows, which had been timing plain paragraphs
  since the async parse went in, read a little higher and are now true.
- **A row that depends on something it does not name.** The cold start row
  calls `openVault` synchronously, so it scales with the vault: 44 ms on 200
  notes, 64 ms on 2 000. This page once gave 47 ms under a 2 000-note heading;
  building that same commit again gave 64 ms, so nothing had regressed.
- **A regression that is really a second copy of a cost.** Cold start went
  from 303 to 518 ms after one change; the change was cheap, but it moved an
  existing 210 ms refresh earlier, where it ran a second time, and made the
  next theme pass pay for a list it had just filled. Phase timings inside
  `openVault` found it in two runs; guessing would not have.
- **A cost that lands after the window closes.** With every feature off, the
  20 000-note cold start read 1 473 ms against 277 ms with them on. Nothing
  off was slow: `processEvents(flags, 50)` checks its deadline only between
  dispatcher passes, and one pass ran the indexer's 6 800 queued results. With
  the panes on, `openVault` took 110 ms longer and the same flood landed just
  after the frame, unmeasured. Counting events by receiver found it; a CPU
  profile could not, because the main thread was mostly waiting.
- **A hypothesis is not a measurement.** Font enumeration was the suspect for
  constructor cost and measured 0.0 ms; the settings page, which nobody
  suspected, was 12 ms.

## Still open

- Two passes per keystroke are still proportional to the note, not the edit:
  `shiftRanges` over every block and decoration, and `updateLayout()`'s one
  placement pass. 0.44 ms in all at 1 MB. Proportional to the edit would take
  offsets stored relative to something, which is a bigger change than the
  numbers ask for yet.
- The Wayland buffer measurement, above.
- PDF page rasters in an editor (`m_pdfPageCache`) are never evicted. Zooming a
  PDF embed adds rasters up to 4x the width, so those alone are bounded (128 MB,
  oldest out first, `PdfEmbedBlock.cpp`); a budget over the ordinary ones would
  want a `QCache`, and a 100-page column re-rendering as it scrolls is the
  cost to weigh. Zoom itself is a repaint of the held raster, no layout.
- Sidebar construction is 7.4 ms of the 15.4 ms constructor, and it is on the
  path to first paint.
- The process's first window is 36 ms to its first frame: fonts (about 5 ms,
  no earlier work to overlap) and `Indexer::rewatch`'s directory walk (6.7 ms,
  see fix 32) are what is left that is not needed for the frame.
- The Tags pane rebuilds in 36 ms the first time it is shown after an index
  change; incremental updates of the tree would remove that.
- A keystroke on the 1 MB note is mostly text shaping of the edited block;
  nothing cheap was left in it.

## 2026-10-16 wave A/B: before the overnight wave (6dd2608) vs after (all lanes merged)

Best of three interleaved runs, 20 000 notes, starbase-1 CCD1, load 7-9. No
row is more than 15% worse. Cold start to first painted frame 35.1 -> 18.8 ms,
1 MB open 12.65 -> 10.57 ms, settings pass 1.41 -> 0.68 ms, keystroke on the
1 MB note 0.50 -> 0.53 ms (budget 0.6), peak RSS 229.5 -> 226.3 MB; cold index
1036 -> 1059 ms (+2%, noise). New rows (tags pane 36.7 ms, notebook open
34.3 ms, resize/fold/media 0.2-1.4 ms, source table Tab 1.9 ms) have no
before value.

## 2026-10-18 index8: saves, renames and the link list

20 000 notes (`tools/gen_bench_vault.py`), starbase-1 CCD1 under load, scan-thread
times as the indexer's own clock reports them.

- An in-app save used to cause two scans: the note's own, then, 300 ms later, the
  watcher's debounced full walk (130-140 ms, nothing to index). The debounce now
  hands the scan only the folders the watcher named (a folder it did not hold yet
  is read whole and watched), so the second scan reads one folder: 30-40 ms, and
  the index is idle 340-380 ms after the save instead of 440 ms. A whole-vault walk
  is left to explicit rebuilds, a setting that changes how notes parse, and the 30 s
  poll while a folder cannot be watched. Edits made in place by another program to a
  note that is not open raise no folder event (inotify on a folder does not report
  them); editors that save through a temp file do.
- `Index::linkTargets()` after a name change: 16.0 -> 3.6 ms (cold 16.8 -> 8.2 ms),
  by reading the names kept in memory (kept in order) instead of a second pass over
  the names table.
- A rename no longer waits for the indexer (it could start a full walk on the UI
  thread and wait up to a second): the notes just saved, and those the indexer has
  been asked to read, are named to the rewrite, which reads them from disk.

## 2026-10-18 watch: edits made in place

`Indexer` reads inotify directly on Linux (`libs/index/DirWatcher`) instead of
through QFileSystemWatcher, whose folder watch reports only entries appearing,
going or being renamed. A program that opens a note, writes it and closes it (no
temp file, no rename) is now seen: the watch adds `IN_CLOSE_WRITE`, and the note
alone goes to the same 300 ms debounce and is reindexed (no folder read, no
whole walk). Still one watch per folder; the fd is read through a
`QSocketNotifier` (no thread, no timer), so nothing wakes while the vault is
idle. `IN_MODIFY` is not used: a program that holds a note open and writes
without closing it is seen when it closes. A file that is not a note (an
attachment, a temp file) is read with its folder, as before. A full kernel queue
(`IN_Q_OVERFLOW`) starts a whole rescan. A folder the kernel will not watch still
turns on the 30 s poll and the status message. Other platforms keep
QFileSystemWatcher (folder events only).

## 2026-10-19 checkbox states

Any one character in a task's box is a state (docs/EDITOR.md, Checkbox states).
What it costs, by the rules above:

- **Off** (`ParseOptions::taskStates` false, the plugin switched off): one `bool`
  branch per list item in `describeListItem` and in `parseListLine`. `taskStateOf`
  answers `[ ]`, `[x]` and `[X]` from a two-entry scan of a function-local static
  table, so no `tasks.ini` is read and no table lookup happens until a task with
  another character is painted. The tasks table of the index keeps one character
  where it kept a flag.
- **On** (the default in the window): the same regex engine with a different class
  (`[^\[\]\n]` for `[ xX]`), one scan of the 13-entry table per custom task per
  layout, one per paint of its box.
- The index schema went 6 to 7 (`tasks.state`), so the first start after the
  upgrade is one cold index.

Measured with `noteAppC-bench` (Release, offscreen) against the commit this was
branched from, built the same way, interleaved. **Indicative only: other lanes
were building on the machine (load average 10-15), so read the ratios.** The
standalone-editor rows run with `ParseOptions` default (feature off); the window
rows go through `MainWindow::parseOptions()` (feature on).

| what | before | after |
|---|---|---|
| keystroke, 1 kB note (off) | 0.52 ms | 0.46 ms |
| keystroke, 200 kB note (off) | 0.70 ms | 0.45 ms |
| keystroke, 1 MB note (off) | 1.16 ms | 0.91 ms |
| keystroke through the window, 200 kB note (on) | 0.30 ms | 0.33 ms |
| open a 1 kB / 200 kB / 1 MB note in the window (on) | 3.0 / 3.9 / 11.9 ms | 2.5 / 3.1 / 11.2 ms |
| cold index, 5 000 notes, bare `Indexer` | 425 ms | 413 ms |
| `OpeningAMegabyteNoteIsImmediate`, 10 runs each, load low | 7.4 ms (200 kB 1.8) | 7.4 ms (200 kB 1.8) |

No row moves outside what two runs of the same binary differ by. The same test
failed about one run in six on the new build while the machine was loaded and
passed ten of ten, alongside the old build, once the load fell.
