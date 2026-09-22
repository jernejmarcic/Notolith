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

2 000-note vault, median of several runs, after every fix below:

| what | median | budget |
|---|---|---|
| window on screen from process start | 50.5 ms | - |
| vault usable from process start | 204 ms | - |
| note open, 1 kB | 1.1 ms | 300 ms |
| note open, 50 kB | 6.9 ms | 300 ms |
| note open, 200 kB | 2.2 ms (readable; styled ~26 ms later) | 300 ms |
| note open, 1 MB | 11.3 ms (readable; styled ~140 ms later) | - |
| keystroke to relaid, painted block, 200 kB | 0.25 ms | 8 ms |
| keystroke, 1 MB | 0.66 ms | 8 ms |
| scroll frame, 1180x760 | 0.36 ms | - |
| scroll frame, 3440x1440 (this machine's 240 Hz panel) | 0.88 ms, worst 2.1 ms | 4.17 ms |
| cold index, 20 000 notes | 3.6 s | 5 s |
| peak RSS, vault plus a 1 MB note | ~200 MB | - |

"Readable" is deliberate wording: above 128 kB a note is shown as plain
paragraphs at once and styled when its parse comes back from the thread pool.
See [the async parse](#8-a-big-note-is-shown-before-it-is-parsed).

## Measuring

```sh
cmake --build build
./build/tools/bench/noteAppC-bench                     # the table, headless
./build/tools/bench/noteAppC-bench --json              # also JSON, for diffing
./build/tools/bench/noteAppC-bench --viewport 3440x1440
QT_SCALE_FACTOR=2 ./build/tools/bench/noteAppC-bench --viewport 2560x1440
./build/tools/bench/noteAppC-bench --gpu               # on a real display: raster vs OpenGL
```

The bench generates its vault with `tools/gen_bench_vault.py` unless one is
given, never touches `~/.config/noteAppC`, and reports median, p95 and worst.

To find where time goes, `perf` works on the real binaries:

```sh
QT_QPA_PLATFORM=offscreen perf record -g --call-graph=dwarf -F 299 \
    ./build/tools/noteAppC_shot <vault> <note> out.png dark 900 600
perf report --stdio --no-children -g none --percent-limit 1.5
```

For throwaway instrumentation use `fprintf(stderr, ...)`. `qWarning` output did
not reach stderr in this environment, and twice that looked like "this code
never runs".

## Guards in the test suite

| test | asserts |
|---|---|
| `MarkdownEditorTest.KeystrokeStaysInsideTheBudget` | under 8 ms per keystroke on a 200 kB note |
| `FindTest.SteppingThroughALongNoteStaysInsideTheKeystrokeBudget` | find-next stays inside the same 8 ms |
| `MarkdownEditorTest.OpeningALargeNoteIsFast` | a 200 kB note opens in under 300 ms |
| `MarkdownEditorTest.OpeningAMegabyteNoteIsImmediate` | time to readable grows slower than the text |
| `ParserPerfTest.ParsingStaysLinearInFileSize` | 5x the text costs under 10x the parse |

The last one used to live in the editor tests. When the parse moved off the UI
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
reason for text - with a steadier tail. The per-frame medians are not
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

1. **Scroll by blitting.** The bench scrolls a whole page per frame, which is
   the worst case. A wheel or touchpad scrolls a few lines, and
   `QWidget::scroll()` moves the pixels already painted and repaints only the
   exposed strip. Pure raster, a small change, and it attacks exactly the cost
   the table above measures.
2. **Measure the Wayland buffer transfer.** The one real argument for a GPU
   surface on this machine is not in the table. A raster window on Wayland
   hands the compositor a shared-memory buffer, which the compositor then
   uploads; a GL or Vulkan surface stays on the GPU. The bench measures
   `QPainter` time and cannot see that copy. Until it is measured, nothing
   says whether it costs frames.
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
- **A hypothesis is not a measurement.** Font enumeration was the suspect for
  constructor cost and measured 0.0 ms; the settings page, which nobody
  suspected, was 12 ms.

## Still open

- The three O(document) passes on every keystroke: the `SourceMap` rebuild,
  `shiftRanges` over every block and decoration, and two full passes in
  `updateLayout()`. 0.25 ms at 200 kB, 0.66 ms at 1 MB - nobody's problem yet,
  but the last thing in the editor proportional to the file rather than the
  edit.
- Scroll by blitting, and the Wayland buffer measurement, above.
- Sidebar construction is 7.4 ms of the 15.4 ms constructor, and it is on the
  path to first paint.
- Nothing has been measured against Obsidian itself: it is not installed here.
