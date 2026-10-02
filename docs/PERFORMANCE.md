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

Median of several runs, after every fix below; a 2 000-note vault unless the row says 20 000:

| what | median | budget |
|---|---|---|
| window on screen from process start | 50.5 ms | - |
| cold start to first painted frame (bench, includes `openVault`) | 32 ms (2026-10-01, 20 000 notes, quiet) | - |
| vault usable from process start | 204 ms | - |
| note open, 1 kB | 1.1 ms | 300 ms |
| note open, 50 kB | 6.9 ms | 300 ms |
| note open, 200 kB | 3.9 ms (readable; styled ~26 ms later) | 300 ms |
| note open, 1 MB | 11.9 ms (readable; styled ~140 ms later) | - |
| keystroke to relaid, painted block, 200 kB | 0.22 ms | 8 ms |
| keystroke through the whole window, 200 kB | 0.17 ms | 8 ms |
| keystroke, 1 MB | 0.44 ms | 8 ms |
| keystroke in a code block, 100 / 1 000 / 5 000 lines | 1.10 / 1.10 / 1.16 ms | 8 ms |
| keystroke in a cell of a 50-row HTML table (2026-10-02, busy machine) | 1.33 ms | 8 ms |
| parse plus HTML structure pass, that note, gumbo cache cold (off the UI thread) | 0.24 ms | - |
| scroll frame, 1180x760 | 0.36 ms | - |
| scroll frame, one wheel notch, on screen (headless cage) | 0.4-0.65 ms | - |
| scroll frame, 3440x1440 (this machine's 240 Hz panel) | 0.88 ms, worst 2.1 ms | 4.17 ms |
| cold index, 20 000 notes | 0.95 s (2026-10-01, quiet; the 1.24 s of stage 14 was the busy machine) | 5 s |
| peak RSS, vault plus a 1 MB note | 227 MB (2026-10-01) | - |

"Readable" is deliberate wording: above 128 kB a note is shown as plain
paragraphs at once and styled when its parse comes back from the thread pool.
See [the async parse](#8-a-big-note-is-shown-before-it-is-parsed).

## Measuring

```sh
cmake --build /tmp/noteAppC-build
/tmp/noteAppC-build/tools/bench/noteAppC-bench                     # the table, headless
/tmp/noteAppC-build/tools/bench/noteAppC-bench --json              # also JSON, for diffing
/tmp/noteAppC-build/tools/bench/noteAppC-bench --phase open        # one phase: memory, start, open, keystroke, scroll, pdfview
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
- Sidebar construction is 7.4 ms of the 15.4 ms constructor, and it is on the
  path to first paint.
- Nothing has been measured against Obsidian itself: it is not installed here.
