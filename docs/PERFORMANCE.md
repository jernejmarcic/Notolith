# Performance

What noteAppC costs, how that was measured, how it stays fast, and the
things that were tried and not adopted. Every number here was measured on
a Ryzen 9 9900X desktop (12 cores / 24 threads, RTX 5080, CachyOS; "the development machine" below) with the
default RelWithDebInfo build (`-O2 -g`) unless it says otherwise, and every one
can be reproduced with the commands in [Measuring](#measuring).

The standing requirement: noteAppC must be faster than Obsidian in
every way, being native C++, and stay fast on a machine with no GPU at all.

## The rules this work follows

1. **Measure before changing anything.** Every optimisation here was found with `perf`
   or the bench, not guessed. Twice a plausible guess was wrong: font
   enumeration was suspected at startup and cost 0.0 ms, and the MicroTeX gap
   was recorded as font coverage when it was a 1 pt raster.
2. **A budget nobody measured is not a budget.** The index budget was only
   ever extrapolated, and was out by a factor of eight.
3. **Guard with ratios, not wall-clock numbers.** Five times the text must
   cost about five times the work. A quadratic cannot hide inside a ratio
   however fast the machine is; it hides easily inside a generous timeout.
4. **Perceived speed never costs more than 10 ms of real speed.** That is the
   limit. A trade of real time for apparent speed has to be measured A/B.
5. **Nothing optional runs before the first paint or on the keystroke path.**
   Preloading is fine once the visually important parts are on screen.
6. **Numbers taken on a busy machine are not numbers.** See
   [Measurement pitfalls](#measurement-pitfalls).

## Handwriting

The `ink` bench phase (`noteAppC-bench --phase ink --json`; `Bench.cpp`, "handwriting (phase `ink`)") measures the
handwriting tab (libs/ext/ink, design in docs/INK-DESIGN.md). The design that the rows
test: the live stroke is rasterised per segment into an overlay image, committed items come from 512 px tiles rendered
on the thread pool (a paint never renders: it paints the tiles it has, blurry or the page colour where it has none, and
asks the pool for the rest), a file above 256 KB is parsed off the UI thread, and the module scan runs after the first
paint. Nothing of it runs until an ink tab is opened.

How the rows are taken: a 1200x800 canvas (the scroll rows: inside a 1200x800 scroll area) holding a synthetic page of
30-point squiggles at one density (a cell is 12 x 40 px; 10 000 strokes make a 4 000 px page, 50 000 a 20 000 px one);
the pen row is 5 strokes of 400 samples at 200 Hz, each move timed as `sendEvent` plus `processEvents`, with the paint
counted to prove it ran; the scroll row steps one viewport height at a time down and back up over the same 4 000 px
once the tile cache is warm (the first two passes untimed; the cold rows use an 8 MB cache and steps two screens
apart, so every step needs new tiles); the zoom row holds the cache to one screen so "nothing in flight" is exactly
"the visible tiles are rendered", and times from the moment the canvas switched its tiles to the new zoom; the module
rows alternate "present" and "empty" sample by sample. Every number above is the median of the medians of three
invocations, run as `taskset -c 6-11,18-23 chrt --idle 0 systemd-run --user --scope -q -p MemoryMax=8G`
(CCD1, SCHED_IDLE, 8 GB), in an isolated XDG home.

**The rows are provisional.** A Wine game (`GameThread`, about 650 % CPU, all 24 cores allowed) ran for the
whole session, CCD1 was 71-77 % idle, and under `chrt --idle` the bench yields to it, so the pool-side rows (tile
latency, zoom re-render, cold start) are pessimistic and noisy; the rows nearest their gate are the cold-start
difference and the keystroke ratio. Both use `--phase inkmodules` (61 alternated pairs, 9 rounds of 30 keys, spread
reported in the row, and a guard that the "present" condition finds the module manifests and the "empty" one finds
none) and read 0.09 ms with quartiles of about +-0.5 ms, and a ratio of 1.00 over three runs (rounds 0.90-1.16). One
caveat is recorded rather than removed: the ink feature registration is process-global (`Settings::registerFeature`),
so after the first "present" window every "empty" window of the same process already has the feature row; "empty"
means no manifests to scan, which is what the scan costs. A rerun on an idle machine would tighten every row of this
section.

Also measured, not gated (`--phase inkcarry`, same conditions): tile render latency for a screen of cold
tiles 32.5 ms at 10 000 strokes and 32.7 ms at 50 000 (a frame that asks for them: 2.1 / 2.3 ms on the UI thread,
which includes building the tile jobs); `selectItems` over 100 050 segments inside an 800-vertex lasso 7.4 ms, over
3 000 shapes 10.9 ms; one sample of dragging a selection of 9 600 points 0.32 ms; one eraser sample over 3 000 shapes
0.077 ms; 50 text boxes on one screen 2.0 ms for the first paint and 1.9 / 1.4 ms for the paint after a zoom 2 / 4
settle; a keystroke in an open box of 16 384 characters 6.1 ms; pen-up in a 50 000-stroke page through the view
1.8 ms; parsing a clipboard payload 95 ms at 7.5 MiB and 187 ms at 15.2 MiB (about 12 ms a MiB, on the UI thread).

Three limits came out of those rows, each with its test:

| what | before | after |
|---|---|---|
| a fast scroll over 50 000 strokes (20 viewport steps without waiting): time until the last screen is sharp, tile jobs that ran | 229.6 ms, 90 jobs | 70.0 ms, 28 jobs (`TileCache::keepOnly`: a paint cancels the queued renders for cells it no longer wants; a cell wanted again after being cancelled gets a new job at once and the abandoned result is dropped) |
| a clipboard payload of 62 MiB pasted | parsed on the UI thread, about 0.75 s (11.9 ms a MiB from the 7.5 and 15.2 MiB rows) | refused by its size at once; the cap is 16 MiB (`kMaxClipboardBytes`) |
| one paint of 8 text boxes of 8 MB each | all 8 rendered and held, 64 MB | rendering stops once the pictures this paint rendered fill the 32 MB budget (a test: at most the budget plus one picture); pictures already cached do not count against it, and a paint that skipped boxes schedules one more, so every box gets its picture within a few paints; all cached pictures together are held under a hard ceiling of 4 x the budget, 128 MB (`kTextCacheCeilingBytes`): over it a box keeps its stale picture or the placeholder and asks for no more paints (a test: 100 boxes of 8 MB in view, painted 12 times, never above the ceiling plus one picture, and the paints stop) |

Cold start and executable size: `ENABLE_EXPORTS` makes the executables carry their whole dynamic symbol table so
modules can resolve host classes (`.dynsym` 2.9 k symbols without it, 15.5 k with it). `noteAppC-bench` from builds
with and without it, `--phase start`, three times alternately, `NOTEAPPC_MODULE_DIRS` an empty directory: cold start to
first painted frame (later windows) 45.08 ms without, 46.01 ms with (+2.1 %), the first window 102.5 and 100.3 ms
(-2.2 %); an earlier set with the module variable unset gave 37.21 and 36.40 ms (-2.2 %) and 87.55 and 88.36 ms
(+0.9 %). The dynamic loader itself (`LD_DEBUG=statistics`, `--help`, five runs each) reports 17.28 million cycles
without and 17.27 million with, so the table costs nothing measurable at start. Size: `stat` 288 271 616 bytes
without, 292 070 216 with (+1.3 %, mostly DWARF at RelWithDebInfo); the text segment (`size`) 12.56 MB to 13.89 MB
(+10.6 %, +1 330 331 bytes). The `stat` figure is dominated by DWARF, which the export table does not touch. Stripped,
the executable grows 12 725 920 to 14 053 376 bytes (+10.4 %); `.gnu.hash` + `.dynsym` + `.dynstr` grow from 0.37 MB
to 1.56 MB (+1 193 117 bytes, +9.5 % of the base text), and everything else in the text segment grows 137 214 bytes
(+1.1 %). The gate passes as worded (on `stat` size, +1.3 % against +10 %), and the export table alone sits just under
the 10 % line when the same gate is read on the stripped executable or the text segment: a further 0.5 % of exported
symbols would cross it. If the line is ever read that way, that is the place to cut (export only what modules
resolve).

## Where it stands

Median of several runs; a 2 000-note vault unless the row says 20 000. Several rows were taken on a loaded
machine, so read ratios more than single milliseconds:

| what | median | budget |
|---|---|---|
| window on screen from process start | 50.5 ms | - |
| cold start, the process's first window (bench, includes `openVault`) | 36.3 ms | 40 ms |
| cold start to first painted frame, later in-process windows (bench) | 19.5 ms | - |
| pop-out window: command to first painted frame (bench, 2 000 notes) | 10.7-11.8 ms (three runs' medians) | 25 ms |
| move a tab to a new window (bench, same) | 3.1-3.4 ms (includes the empty tab main gets in its place) | 5 ms |
| vault usable from process start | 204 ms | - |
| note open, 1 kB | 1.8 ms | 300 ms |
| note open, 50 kB | 7.9 ms | 300 ms |
| note open, 200 kB | 2.5 ms (readable, styled ~26 ms later) | 300 ms |
| note open, 1 MB | 10.2 ms (readable, styled ~140 ms later) | - |
| note open, notebook with 40 stored PNG outputs | 37.7 ms | - |
| Tags pane refresh, 20 000 notes, 4 505 distinct tags | 36 ms when shown; 0.00 ms while hidden (marked stale, rebuilt on show) | - |
| every note for a base (`allNotesForBases`), 20 000 notes, off the UI thread | 33 ms (65 ms before) | - |
| keystroke to relaid, painted block, 200 kB | 0.22 ms | 8 ms |
| keystroke through the whole window, 200 kB | 0.17 ms | 8 ms |
| keystroke, 1 MB | 0.55 ms (0.44 ms on a quiet machine) | 0.6 ms, 8 ms |
| source-mode table Tab, 1 MB | 2.0 ms | - |
| resize, text width kept / rewrapped, 1 MB | 0.24 / 1.36 ms | - |
| fold a callout, 1 MB | 0.29 ms | - |
| fold all / unfold all headings and list parents, 1 MB (10 958 folds: 5 479 headings, 5 479 list parents) | 1.81 / 1.52 ms | 50 ms |
| `foldState()` after fold all, 1 MB (10 958 folds; only asked on leaving the note) | 2.18 ms | 50 ms |
| keystroke on a heading with everything folded, 200 kB | 0.15 ms | 8 ms |
| keystroke in a code block, 100 / 1 000 / 5 000 lines | 1.10 / 1.10 / 1.16 ms | 8 ms |
| keystroke in a cell of a 50-row HTML table | 1.33 ms | 8 ms |
| parse plus HTML structure pass, that note, gumbo cache cold (off the UI thread) | 0.24 ms | - |
| scroll frame, 1180x760 | 0.36 ms | - |
| scroll frame, one wheel notch, on screen (headless cage) | 0.4-0.65 ms | - |
| scroll frame, 3440x1440 (this machine's 240 Hz panel) | 0.88 ms, worst 2.1 ms | 4.17 ms |
| scroll by finger on a FydeTab Duo (RK3588S, GNOME at 1.67, OpenGL by default there), a real note, in its own session | 60 fps: frame gap 16.6 ms median, paint 3.8 ms | 16.7 ms |
| Settings, first open in a window (bench) | 30 ms (252 ms before) | - |
| cold index, 20 000 notes | 0.95 s on a quiet machine; 1.04 s with the tag-heavy note | 5 s |
| peak RSS, vault plus a 1 MB note | 228 MB | 250 MB |
| ink pen sample, 10 000-stroke page (bench) | 0.047 ms (provisional; runs 0.047, 0.047, 0.047) | 1 ms |
| ink scrolled frame, 10 000 strokes (bench) | 1.03 ms (provisional; runs 1.01-1.08) | 4 ms |
| ink scrolled frame, 50 000 strokes, ratio to 10 000 | 1.07x (provisional; 1.10 / 1.03 ms; per-run ratios 1.01-1.09) | 2x |
| ink zoom settle to sharp (bench), the re-render after the tiles switch | 20.8 ms (provisional; runs 20.7-22.2; plus kZoomSettleMs = 400 ms, 403.9 ms in all) | 100 ms |
| cold start, module directory present vs empty | 0.09 ms paired median difference (provisional; three runs of 61 alternated pairs: medians 0.09, -0.03, -0.03; quartiles about -0.5 .. +0.55 each; the earlier 21-pair runs of 0.47, -0.37, 1.37 were noise of this size, which is why the count went to 61) | 1 ms |
| window keystroke on the 1 MB note, module directory present vs empty (after the scan), ratio | 1.00x pooled (provisional; three runs of 9 rounds: 0.58 / 0.57 ms; the rounds' own ratios range 0.90-1.16, 0.94-1.16, 0.97-1.04) | 1.10x |
| cold start, with vs without the symbol export (`ENABLE_EXPORTS`) | +2.1 % later windows, -2.2 % the first window (provisional); the loader's own time equal | +5 % |
| `noteAppC` executable size, with vs without the symbol export | +1.3 % file (292 070 216 vs 288 271 616 bytes, the gate); +10.4 % stripped, +10.6 % text segment, of which the dynamic tables are +9.5 % of the base text (13.89 vs 12.56 MB; just under the line on those readings) | +10 % |

"Readable" is deliberate wording: above 128 kB a note is shown as plain
paragraphs at once and styled when its parse comes back from the thread pool.
See [Opening a note](#opening-a-note).

## Against Obsidian

Head-to-head on the development machine: Obsidian 1.14.4 (Electron 43, no
plugins, never signed in) against noteAppC (Release). Both ran in
the same headless `cage` (1280x720, real RTX 5080 through GL, so Obsidian was
GPU-accelerated), in a throwaway `HOME`/XDG tree, pinned to CCD1 (cores
6-11,18-23) at `nice 19`, one app at a time, interleaved (noteAppC cold, noteAppC
warm, Obsidian cold, Obsidian warm, then the next round). Every cell is the
**median of 5**, and the machine was not quiet (load average 2 to 5, other
builds running at idle priority), so read ratios, not milliseconds.

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
  1.28 s / 2.18 s). The Tags pane now reads every tag count in one query
  (`Index::tagCounts()`, see "Indexing and queries"), and the bench vault
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
cmake --build build
build/tools/bench/noteAppC-bench                     # the table, headless
build/tools/bench/noteAppC-bench --json              # also JSON, for diffing
build/tools/bench/noteAppC-bench --phase open        # one phase: memory, start, open, keystroke, save, relayout, scroll, pdfview
build/tools/bench/noteAppC-bench --viewport 3440x1440
QT_SCALE_FACTOR=2 build/tools/bench/noteAppC-bench --viewport 2560x1440
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    cage -- build/tools/bench/noteAppC-bench --gpu    # raster vs OpenGL, headless
build/tools/bench/noteAppC-bench --fydetab              # the FydeTab Duo's scroll: scale 1.67, finger/wheel/page steps
build/tools/bench/noteAppC-bench --fydetab --note my.md --size 685x1278  # a real note, the FydeTab Duo's portrait editor
```

`--fydetab` (the FydeTab Duo's setup) styles the window as the app does (PopupStyle and the theme's sheet,
so the scrollbar costs what it costs there) and scrolls at 20 px (a finger),
72 px (a wheel notch) and page steps. It sets `QT_SCALE_FACTOR=1.67` unless
one is set; set 2 to see what an integer scale does to the same note.
`--note` scrolls that file instead of the generated one, `--size` sets the
viewport. Offscreen it measures the paint only, never the compositor.

For a device's own session, where the compositor and the input are real,
`NOTOLITH_FRAME_LOG=<file> notolith` appends one line per editor paint: gap
since the last paint, paint time, exposed and viewport size, device pixel
ratio, scroll steps since the last paint, the time spent handling them, and
the wait from the first of them to the paint. A frame bound by the handler,
the paint or the wait each needs a different fix; the FydeTab Duo scroll work
read them in that order (see "Scrolling and GPU acceleration").

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
is tmpfs on the development machine and `fdatasync` there costs nothing (0.14 / 0.48 ms
median, which says nothing about a disk). The bench also points
`XDG_DATA_HOME` at a private directory, so the snapshots stay out of the real
`~/.local/share/noteAppC/recovery`.

```sh
mkdir ~/.cache/bench-tmp && TMPDIR=~/.cache/bench-tmp \
    build/tools/bench/noteAppC-bench --phase save
```

| save | median | p95 / worst | where |
|---|---|---|---|
| 200 kB note | 5.7 ms | 15.7 ms | development machine, NVMe (btrfs on LUKS), three runs |
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
    build/tools/noteAppC_shot <vault> <note> out.png dark 900 600
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

Both a Rust (yrs) and a native C++ text CRDT were tried behind one
interface ([SHARE.md](SHARE.md)) and compare them. One suite runs over both;
`test_share_crdt --gtest_filter='*Megabyte*:*SizesAndMemory*'` prints these
numbers (development machine, RelWithDebInfo, quiet machine, load average 0.9; three runs
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
`perf::bestOf`), because a loaded machine - a parallel build, another test run next
door - only ever adds time, so the fastest run is the cost of the code. The
budgets themselves are unchanged, so a real regression still fails all three.
Under ASan or TSan the guards skip (`SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER`):
the clock would measure the instrumentation.

## How it stays fast

The mechanisms behind the numbers above, by area. Each one is pinned by a test
or a bench row (see "Guards in the test suite").

### The keystroke path

- Only the blocks near the viewport are laid out; the rest are estimated. An
  edit is applied incrementally: ranges are shifted, one block is relaid, and
  the parse follows on the thread pool behind a generation counter. Line
  height is memoised per block kind (building a `QFont` and `QFontMetricsF`
  for every block twice per pass was the whole cost of the first engine: 375 ms
  to 0.09 ms on a 200 kB note).
- `SourceMap::applyEdit` moves the line starts after the edit instead of
  rescanning. `updateLayout()` settles and places blocks in one pass, finds
  the blocks near the viewport by binary search, re-places only from the first
  block whose height changed, and repeats until nothing changes (at most 8
  passes). List items and `DisplayMap::toDisplay` / `isHidden` use binary
  search too.
- Between a keystroke and its reparse, the cached `DisplayMap`s and every
  decoration a layout or paint can read (callouts, tables, embeds, maths, `#|`
  options, paragraphs) shift with the text, so blocks below an edit are never
  read at stale offsets.
- A fence longer than 32 lines is split into 32-line blocks
  (`kCodePieceLines`), so an edit relays out one piece. The fence's chrome is
  drawn across its pieces, and the highlighter state (`CodeHighlighter::State`)
  carries from one piece to the next, stopping where it converges again. The
  highlighter builds one `QTextCharFormat` per Kate format instead of asking
  for the theme per token. A keystroke in 100 / 1 000 / 5 000 lines costs
  1.10 / 1.10 / 1.16 ms.
- Whole-note work never runs per keystroke: the word count travels with the
  parse, and Tab or Enter in a source-mode table finds the table in the shifted
  decorations instead of reparsing (340 ms to 2 ms at 1 MB). The bench's
  keystroke row types through the whole `MainWindow` for this reason.
- A resize relays out only when `contentWidth()` changed, a fold invalidates
  that one block, and a link turning resolved invalidates only blocks that link
  to the target. A formatting-colour change (`Theme::sameExcept`) drops the
  layouts but keeps every height.
- Every settings pass compares the theme and restyles only when it changed
  (21.6 ms to 0.7 ms with a 1 MB note open).

### Opening a note

- Above 128 kB a note goes up at once as plain paragraphs
  (`plainParagraphs()`) and the parse runs on the thread pool; the styled
  version replaces it when it arrives (200 kB: 25.7 to 2.2 ms to readable,
  1 MB: 146 to 11.3 ms). Below 128 kB the parse is faster than a frame and
  stays synchronous, so there is no unstyled flicker. Callers that must hold
  the parsed note (a canvas card's grab, a PDF export) call
  `MarkdownEditor::finishParse()`, which waits on the pool and adopts the
  result; they must not spin a nested `QEventLoop`, because the canvas card is
  rendered inside `paintEvent`.
- `ObsidianSyntax::scan` finds `%%...%%` comments in one pass over the whole
  text and assigns matches to regions by binary search (a lazy pattern run
  once per region was quadratic on a 1 MB note). Panes read the note once per
  refresh, not once per link.
- `rebuildBlocks` builds its `BlockView`s (1.6 kB each) in a vector sized for
  them, so growth never moves them.
- A notebook's stored outputs are seeded as one closure per cache key and
  decoded the first time a laid-out block looks them up; a run or a clear
  before then drops the closure (40 PNG outputs: 351 to 35 ms).
- Backlinks cost nothing in the vault size: links carry `tname`, the target's
  last path component folded and without the extension, indexed, and a target
  can only resolve to a note with that name. Unlinked mentions read only the
  first 16 kB of a candidate (more only if its front matter is still open) and
  parse only the front matter.

### Start-up

- The window is shown first and the vault is read second (`openWhenShown()`
  waits for the empty shell's first paint): on screen at 50 ms instead of
  194 ms, for 8.8 ms of real cost because the shell is painted twice. That is
  inside the 10 ms rule, but anything further has to make the first paint
  cheaper rather than add another.
- Work is not done until it is needed. The settings pages are built on their
  first `showEvent`, one page at a time, with the style sheet set before the
  rows exist (first open 252 to 30 ms). Each editor's `CodeHighlighter` builds
  its syntax repository on the first `knows()` or `highlight()` (2.3 ms each,
  and a canvas builds an editor per visible node). libgit2 is not initialised
  until history is first used (the init is 3-4 ms and 6.2 MB, mostly OpenSSL).
- The theme is applied to the application before any widget is built, and
  `applyTheme()` sets the palette and the style sheet only when they differ,
  because setting an unchanged sheet repolishes every widget.
- The `[[` suggester sets uniform item sizes: without them an application-wide
  style change measured 60 000 rows (157 ms of a 20 000-note cold start).
- The panes that listen to `Index::changed` refresh once per change, and a
  hidden pane (graph, tags) only marks itself stale and catches up in
  `showEvent()`.
- The indexer's directory watches come from the scan's own walk, each with the
  mtime it read; a directory whose mtime moved before its watch started
  triggers a rescan, and only the difference is (re)watched. A folder inotify
  will not watch (past `fs.inotify.max_user_watches`, or no permission) is
  reported once in the status bar, and the indexer rescans every 30 s while any
  folder lacks a watch.

### Indexing and queries

- A scan - walk, `isCurrent`, attachments, removals and writes - runs on a
  thread of its own with its own connection (`Index::Writer`), with statements
  prepared once. The UI thread takes only each 256-note chunk's names and
  progress, and one `changed()` at the end. A scan that `stop()` cancels drops
  what it queued, so its names never reach the next vault's table.
- The full-text table is deleted by an indexed key; a non-indexed column made
  every note scan the whole table (20 000 notes: 39.8 s to 3.6 s, later 1 s).
- `Index::search()` sorts `rowid, path` only and reads a body by rowid while
  fewer than `limit` notes have matched (a query matching every note: 18.1 to
  6.4 ms).
- `Index::tagCounts()` reads the (note, tag) pairs once and adds each note to
  its tag and every `/`-parent (4 505 tags: 9.5 s of `notesWithTag()` calls to
  38 ms).
- `allNotesForBases` uses forward-only queries and reads the three child
  tables on their own threads and connections (WAL lets readers run side by
  side), with `std::async` because the caller may itself be on the global pool
  (65 to 33 ms). `GROUP_CONCAT` per note was slower.

### Scrolling and GPU acceleration

`QAbstractScrollArea` repaints everything on any scroll. The editor overrides
`scrollContentsBy`: when the step is smaller than the viewport and the layout
that runs with it moved nothing on screen, `viewport()->scroll()` moves the
painted pixels and only the exposed strip is painted, and `paintEvent` finds
the blocks in that strip by binary search (a wheel notch 1.00 to 0.40-0.65 ms,
headless cage, 1180x760). A page step, a layout that moved something visible
and the OpenGL viewport repaint whole. A step whose reach (a screen either
side) is already laid out skips `updateLayout()`, and while the touch scroller
moves the hover ring is not re-aimed. `ScrollTest.AScrolledFrameMatchesAFreshOne`
compares the backing store after odd-sized scrolls against a full repaint.

At a fractional device pixel ratio Qt cannot blit: `QBackingStore::scroll`
refuses a non-integer native delta and Qt Wayland's shm store refuses any
scroll at a fractional scale, so every frame repaints the viewport and uploads
it. The OpenGL viewport has no shm upload. `editor.gpuAcceleration` is
therefore `auto`, `true` or `false`, and `auto` takes OpenGL exactly where
raster cannot blit, a fractional ratio (`MarkdownEditor::GPUaccelerationWanted`,
pinned by `GPUaccelerationTest.AutomaticIsOnOnlyAtAFractionalScale`). Where
raster can blit it is faster: on a Radeon 780M 2-in-1 laptop at scale 1 a 72 px
step is 0.96 ms raster against 2.37 ms OpenGL, a page 2.24 against 3.34 ms.
On a FydeTab Duo (RK3588S, Mali-G610, 2560x1600 at GNOME's 1.67) OpenGL scrolls
a real note at 60 fps with a 3.8 ms paint and a 16.6 ms frame gap. The touch
scroller's `DragStartDistance` is 2 mm (Qt's 5 mm default made the page stand
still under the finger), and `Theme::firstAvailableFamily` / `hasFamily` cache
the family set, dropped on `fontDatabaseChanged`, because
`QFontDatabase::families()` rebuilds its list on every call.
`PopupStyle::polish` grabs the touch gesture for every scroll area that is not
inside another, so the settings page, the file tree and the panes scroll by
finger.

### The graph view at 20 000 notes

A step of the force layout is O(n log n) and a frame is linear in nodes and
edges; at 20 000 notes and 40 000 links a step is 22-28 ms and a frame 18 ms,
so both run off the UI thread. Stepping and painting are one `QtConcurrent` job
at a time, painted into a `QImage`; the UI thread draws that image (and,
between jobs, maps it from the camera it was painted with to the current one,
so pan and zoom are instant). The index read, link resolution and node/edge
building run on a worker over a snapshot (`Index::graphDataIn`, the same
pattern as `searchIn`); a rebuild keeps every surviving node where it was.
Threading contract: GraphView.h.

| 20 000 notes / 40 000 links, best of three | on the UI thread | on a worker |
|---|---|---|
| layout step | 28.5 ms | 22 ms (run-to-run noise) |
| frame | 17.9 ms | 8.5 ms |
| UI thread per paint | 17.9 ms | 0.2 ms |
| `setIndex()` blocks the UI | 123 ms | 0.0 ms |
| longest UI stall while the graph loads | 123 ms | 6.8 ms |
| graph ready after `setIndex()` | 123 ms | 83 ms |

`GraphPerformanceTest.TwentyThousandNodesReportCosts` prints the first four;
`BigVaultOpenCosts` (set `NOTEAPPC_GRAPH_BENCH_NOTES=20000`) prints the rest. A
save while the graph is visible waits for the job in flight before it
rebuilds: one job, about 35 ms at 20 000 notes, but up to 250 ms with Animate
off, which runs a settling pass per job.

### The EPUB reader

Measured by `tests/test_epub.cpp`'s own `Perf*` tests, printed via
`fprintf(stderr, ...)` (`QT_FORCE_STDERR_LOGGING=1 ctest -R epub`), not by the
bench.

- **Chapter rendering** runs litehtml's layout and raster (about 11 ms) on
  `QtConcurrent::run`, delivered back through the same Lifeline +
  generation-counter pattern (`m_renderEpoch`) the editor's embeds use
  (`PdfEmbedBlock.cpp`). The archive, CSS and font work stays on the calling
  thread: it is cheap, and `QFontDatabase::addApplicationFontFromData` is not
  safe off the GUI thread. A cache miss during paint shows the known or guessed
  chapter height as a placeholder and repaints when the worker delivers. The
  work costs the same; it no longer blocks the UI thread.
- **The table of contents** is built the first time the panel is shown, one
  level at a time (a node with children gets one placeholder row until it is
  expanded, `onTocExpanded()`). On a synthetic 50 000-entry TOC `open()` costs
  0, the first show 346 us, one volume of 1 000 rows 429 us, the other 49 000
  rows 20.1 ms (what an eager build at `open()` would pay).
- **CSS `url()` resources** in stylesheets and inline `<style>` blocks are
  extracted from the archive like an `<img src>` (`rewriteCssUrls`), and every
  `@font-face` source is registered once per archive path
  (`registerChapterFonts`). Remote sources are never fetched.
- **Paged mode** shows one or two page-height slices of the same continuous
  litehtml layout (`m_vscroll` snapped to multiples of the viewport height,
  two columns from `kTwoColumnMinWidth`, 900 px), so the render cache,
  hit-testing and highlight painting work in both modes. A page turn is a
  position update (0 ms).

### Smaller things

- `NoteHistory`'s constructor only stores the path; the first call that needs
  the repository opens it (the first save, on the history pool).
- `Vault::decode` skips the CRLF replace for text with no `\r`.
- A canvas's cold first paint tracks the visible nodes, not the total: the
  guard compares a 1 000-node canvas with one of 60 nodes that all fit the
  viewport (about 1x; a broken cull is about 16x).

## Tried and not adopted

### OpenGL viewport

`MarkdownEditor::setGPUacceleration` swaps the viewport for a `QOpenGLWidget`,
which is the supported way to put widget painting on the GPU without QML. At an
integer scale it measured slower, so it is off there; at a fractional scale it
is on by default, where raster cannot blit (see "Scrolling and GPU
acceleration").

On screen, vsync off, 5 000-line note, three runs:

| viewport | whole scroll sweep | median frame | worst frame |
|---|---|---|---|
| raster | ~420 ms | 1.05-1.72 ms | 11.6-12.0 ms |
| OpenGL | ~467 ms | 2.16-2.20 ms | 5.1-5.3 ms |

About 11% slower overall - the GL paint engine's glyph cache is the usual
reason for text - with a steadier tail.

Headless in `cage`, where both viewports get the same virtual output and
near-equal frame counts (420 and 444), so frame by frame is fair:

| viewport | median frame | worst frame |
|---|---|---|
| raster | 0.70-0.98 ms | 5.3-5.6 ms |
| OpenGL | 1.57-1.68 ms | 4.3-4.8 ms |

The same verdict: slower on average, slightly steadier in the tail. The
per-frame medians are not comparable on their own, because the GL viewport
scrolled in larger steps: 216 frames against 400. The whole sweep is the fair
number.

### Vulkan

Qt 6.11 has Vulkan and `QRhiWidget`, so a Vulkan path is buildable. It is not a
switch, though: there is no Vulkan paint engine for `QPainter`. Vulkan in a
widget application means `QRhiWidget` plus a renderer of our own - a glyph
atlas built from `QRawFont::alphaMapForGlyph`, glyph positions taken from the
`QGlyphRun`s the existing `QTextLayout`s already produce, one instanced-quad
pipeline, and shaders compiled with Qt's `qsb` so the same source serves
Vulkan, OpenGL and Metal. Every panel, caret, selection, image and callout the
editor paints today would have to be redrawn that way.

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

The development machine's main panel is 3440x1440 at 240 Hz, a 4.17 ms budget,
and raster fits it with room. The editor's column is capped at a readable
width, so most of a wide window is flat background, which is cheap. Raster
stops keeping up at a 5K panel at 2x (6.9 ms, over a 144 Hz budget) and falls
below 60 Hz at 6K and beyond.

Scrolling already blits, so a wheel notch repaints only the strip that came in.
The remaining cost raster has is the Wayland buffer transfer. As a proxy, the
compositor's own CPU time per frame (`perf stat -i` on `cage`, its threads
only, minus a run that opens the window and scrolls nothing, user time, 1180x760)
is 0.28-0.31 ms for raster (shared-memory buffer, uploaded) against 0.065 ms for
OpenGL (the buffer stays on the GPU). The copy is real, roughly 1.4 ms at
3440x1440 if it scales with the pixels (an estimate, not a measurement), but
raster still costs less in all, app and compositor together, than OpenGL does at
this size. That is cage's wlroots GLES2 renderer, not Hyprland. Reproduce with
`noteAppC-bench --gpu --only none|raster|opengl` under
`perf stat -i -e task-clock -- cage -- ...`.

A Vulkan renderer is only worth building if HiDPI panels matter. It would sit
behind the same `editor.gpuAcceleration` setting; none is built.

## Measurement pitfalls

Each of these produced a wrong number at least once in this project.

- **Contention.** The first bench run measured a 1 MB keystroke
  at 13.7 ms, over budget, while two other builds were running. On a quiet
  machine it was 0.78 ms.
- **Rendering a GL widget into a `QImage`** reads its framebuffer back over the
  bus and measures the readback, not the drawing. The GPU comparison had to be
  on screen, with vsync off, or every sample comes back as one refresh.
- **A frame captured at logical size.** `paintedFrame()` built its image at the
  widget's logical size, so a 2x run painted a quarter of the pixels and called
  it 2x. It surfaced because the 2x and 1x numbers came back identical.
- **Unequal step counts.** Two viewports scrolling the same document in
  different step sizes cannot be compared frame for frame. Compare the sweep.
- **A guard that stops guarding.** Moving the parse off the UI thread changed
  what the editor's linearity test measured, without failing it. When the code
  path under a test changes, check the test still exercises the thing it was
  written for.
- **A benchmark that skips the real path.** The keystroke row drove a bare
  editor, so a whole-document word count the shell ran on every keystroke was
  invisible to it for as long as it existed.
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
