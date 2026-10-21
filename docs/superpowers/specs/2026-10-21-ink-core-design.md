# Ink core and loadable modules: design (Spec 1 and 1b)

Date: 2026-10-21. Status: approved in conversation section by section; this
document awaits Jernej's review before the implementation plan is written.

Notolith gets handwriting: pen input, whole handwritten notes, sketch sections
inside Markdown notes, pen markup over Markdown text, PDF, images and EPUB,
local handwriting recognition, and a handwriting font made from his own
writing. This spec covers the first two cycles:

- **Spec 1**: the loadable-module mechanism, the editor seams, the `.ink`
  format and the ink core (A), with a basic handwritten-note tab (B without
  `.rnote`).
- **Spec 1b**: the glyph studio, in parallel with Spec 1 from milestone A0.

Every later piece has its own spec. The decisions they inherit are fixed in
section 2, so nothing here has to be re-decided later.

Research and decision records, all in the second brain session
`sessions/2026-10-21_handwritten-notes/`:
- NOTES.md: Jernej's decisions, verbatim.
- DESIGN-DRAFT.md: the planner draft this spec is built from.
- rnote-study.md, sol-ink-qt.md, sol-ink-modeler.md, sol-hwr.md and
  sol-hwfont.md: research.
- code-map.md and plugin-map.md: maps of the code this builds on.

## 1. Intent

What Jernej asked for:

- Two equal uses: fully handwritten notes, and handwritten sections inside
  normal Markdown notes. Plus circling and scribbling on Markdown, PDF "and
  such".
- An own open format, and `.rnote` editing. Rnote is the reference design.
  It is GPL-3.0, as is Notolith, so its code can be reused with attribution.
- Built "block by block": "a user can have a hyper minimal, ultra fast
  workflow that has nothing but text". Every non-essential feature is a
  plugin that "shouldn't even need to be installed".
- The handwriting font is a separate plugin. It is "kind of trained", with
  several variants of each letter so the text looks handwritten. A glyph
  studio, where you write a letter and it is saved, comes early so he can
  test it.

Success criteria:

1. **Without the ink packages**, the app behaves and benchmarks exactly as
   today.
2. **Installed but unused**: cold start to first paint, and a keystroke on
   the 1 MB note, are unchanged within noise.
3. **Pen to pixel**: well inside one frame. Handling one pen event costs
   under 1 ms of CPU at 200 Hz on shuttlepod-1 and PADD.
4. **Large pages**: a page of 10 000 strokes scrolls at the display rate.
5. **Open format**: `.ink` is plain and documented. A second program could
   read it from section 5 alone.

## 2. Decisions for the whole roadmap

All decisions were approved by Jernej on 2026-10-21. Later specs build on
them without re-asking.

### Input

| Topic | Decision |
|---|---|
| Pen | Draws. The eraser end erases. The lower barrel button is a held eraser and the upper one a held lasso (Rnote's defaults). |
| Touch | Scrolls and pinch-zooms. It never draws. |
| Mouse | Draws only while mouse inking is explicitly on. Otherwise it behaves as it does now, inside ink areas too. |
| Smoothing | ink-stroke-modeler is a required dependency of the ink module, and smoothing is on by default. Raw input is a setting at most. |
| Undo | Every ink surface has an undo stack and a redo stack. Redo is cleared by a new edit. |

### Files and storage

| Topic | Decision |
|---|---|
| `.ink` format | Plain JSON, one stroke per line. The reader sniffs the gzip magic bytes (`1f 8b`) from v1, so a compressed variant can be added later without breaking existing files. |
| Paper | Chosen per note from Rnote's four layouts. The default is A4 pages stacked downward, with a page added as the ink grows. |
| Typed text box | It is the real Markdown editor (canvas text-card pattern), so links, tags and formatting work in it. |
| Search | Text boxes are indexed now. Recognised handwriting is indexed later, by Spec 6. |
| `.rnote` | Opened, edited and saved as `.rnote`, with lossless pass-through of anything the model does not know. "Save as .ink" and "Export as .rnote" are commands. |
| Companion files | One hidden companion file per host file under a tidy folder, `Sketches/` by default, mirroring the vault path:<br>- `Uni/SCC222/Lecture 3.md` uses `Sketches/Uni/SCC222/Lecture 3.ink`.<br>- `paper.pdf` uses `Sketches/paper.pdf.ink`.<br>It holds the sketch sections and the markup. It follows the host file on rename, move and delete. |
| PDF ink | Stored in the companion file, with pressure, plus a "Flatten into PDF" command. |
| EPUB | Pen highlights use the existing EPUB highlights. Pen marks are anchored to EPUB locators. |
| Images | Keep their existing `annotations.md` companion, with pressure added. |

### Sketch sections and markup

| Topic | Decision |
|---|---|
| Sketch sections | Automatic: writing with the pen in an empty paragraph or below the last line creates the section, the companion file, and an embed line in the note, `![[Sketches/<path>.ink#s1]]`. Source mode shows that embed line; Live Preview and Reading show the sketch. |
| Highlighter | Over text it applies `==highlight==` to the characters it covers and stores no ink. Over handwriting or an empty page it does nothing. |
| Markup anchoring | Per character: a letter covered by the circle or the highlighter counts, even inside a word. A letter only near the stroke does not. The same rule applies to circles and highlights. Thresholds are tunable and kept with a test set of example strokes; they get refined by trial and error. |

### Recognition and font

| Topic | Decision |
|---|---|
| Recognition | Local only. The engine is PP-OCRv6, with Tesseract as the fallback. Results are stored inside the `.ink` file. Hand-written `[[link]]`, `#tag`, headings and checkboxes are detected. Nothing is created without confirmation. |
| Font | Studio samples feed a style model (SDT first, DeepVecFont v2 as the fallback) that produces several variants per glyph. Jernej approves every glyph by eye. fontTools in the Python training worker builds the TTF, and `calt` cycles the variants. |
| Glyph samples | Stored in the vault under `Handwriting/<font>/`, so Syncthing brings them to starbase-1 for training. |

### Delivery

| Topic | Decision |
|---|---|
| Order | Spec 1, with 1b alongside it. Then 2 (`.rnote`), 3 (sketch sections), 4 (markup over text), 5 (PDF, image and EPUB), 6 (recognition) and 7 (font training and build). |

## 3. Blocks and install units

Each block lives under `libs/ext/<name>`, has its own CMake option and its
own runtime feature id, and ships as its own package. Only Spec 1 and 1b
blocks are built by this spec; the rest of the table fixes names so the
seams fit them.

| Block | Directory | Install unit | CMake option | Feature id | Spec |
|---|---|---|---|---|---|
| ink core library | libs/ext/ink/core | `libnotolith-inkcore.so` | `NOTEAPPC_WITH_INK` | - | 1 |
| ink module (handwritten-note tab, later sketch sections) | libs/ext/ink | `notolith-ink.so`, package notolith-ink | `NOTEAPPC_WITH_INK` | `ink` | 1, 3 |
| rnote | libs/ext/rnote | `notolith-ink-rnote.so` | `NOTEAPPC_WITH_RNOTE` | shares `ink` | 2 |
| markup | libs/ext/inkmarkup | `notolith-ink-markup.so` | `NOTEAPPC_WITH_INK_MARKUP` | `ink-markup` | 4 |
| recognition | libs/ext/hwr + tools/hwr_worker | `notolith-hwr.so` + worker | `NOTEAPPC_WITH_HWR` | `handwriting-recognition` | 6 |
| handwriting font | libs/ext/hwfont + tools/hwfont_worker | `notolith-hwfont.so` + worker | `NOTEAPPC_WITH_HWFONT` | `handwriting-font` | 1b, 7 |

Dependencies:
- **One direction only.** Modules link `libnotolith-inkcore.so`. Nothing in
  `libs/editor`, `libs/index` or `libs/ui` links any ink library.
- **Finding the core library.** Modules use RPATH `$ORIGIN`.
- **pdfview, imageview and epub** reach ink at runtime through the module
  host, never by linking. The base binary must load without the package.
- **Missing dependency.** A module whose dependency is absent is reported
  in Settings and not loaded.

## 4. Loadable first-party modules

A new tier sits between today's two. Tier 1 is compiled in and switched off;
Tier 2 runs out of process with no editor hooks. The new tier is
**in-process, Tier 1 hooks, separate shared library, loaded at runtime**.
Ink needs a tablet event turned into pixels every 5 ms inside the editor's
paint, and a pipe cannot do that inside the 10 ms rule.

### The interface

- Modules are Qt plugins (`QPluginLoader`). The interface is
  `NotolithModule` with one method, `attach(ModuleHost &)`.
- `ModuleHost` is a plain struct of the section 6 seams plus three
  registrations: commands, Settings rows, and view factories by file
  suffix.

### The manifest

- `Q_PLUGIN_METADATA` embeds a JSON manifest with these keys:
  - id;
  - feature id;
  - file suffixes;
  - command ids and titles;
  - dependencies;
  - an ABI tag.
- Discovery reads it with `QPluginLoader::metaData()`, which does not load
  the library.

### Discovery and loading

1. **Directories.** `$XDG_DATA_HOME/noteAppC/modules/` first, then
   `<libdir>/noteAppC/modules/`.
2. **When.** The scan runs on the thread pool after first paint
   (PERFORMANCE.md rule 5). It costs a directory listing plus one metadata
   read per module.
3. **What it registers.** The suffixes and commands from each manifest.
4. **Loading.** `QPluginLoader::instance()` loads a module only on first
   use: a file of its suffix opens, one of its commands runs, or one of its
   embeds scrolls into view.

**Gate.** The first bench must verify that the scan does not load the
libraries.

### ABI and settings

- **ABI tag**: same build only, formed as
  `PROJECT_VERSION + "-" + git hash + "-" + QT_VERSION_STR`. A mismatch is
  refused with a notice naming the module. A stable C ABI stays Tier 2's
  job.
- **Settings.**
  - Feature rows for module features exist only when the module is
    installed (the pattern at SettingsPage.cpp:900-931).
  - An installed module defaults to on, because installing the package is
    the opt-in.
  - Turning it off unregisters its suffixes and commands. A loaded library
    is not unloaded mid-session; it stays loaded until restart.

**Cost when absent:**
- no ink symbols;
- the directory scan finds nothing;
- every seam is a null `std::function` or an empty map;
- the editor pays one pointer check per block paint and one per viewport
  event.

**Follow-up, out of scope:** canvas, bases, pdfview, imageview, epub, graph
and publish could move to the same mechanism and shrink the base binary.
That is a separate decision.

**Packaging.** The Arch PKGBUILD grows split packages (`notolith`,
`notolith-ink`, `notolith-hwfont`). The plain CMake install puts modules in
`<libdir>/noteAppC/modules/`.

## 5. The `.ink` format, version 1

UTF-8 JSON, one top-level object. Each stroke goes on its own line, so git
history and sync produce deltas the size of one stroke.

### Envelope

```json
{"format": "notolith-ink", "version": 1,
 "document": {"format": {"width": 793.701, "height": 1122.52, "dpi": 96},
              "background": {"color": "#ffffff", "pattern": "lines",
                             "patternSize": 32, "patternColor": "#c0c8d0"},
              "layout": "continuousVertical",
              "extent": {"x": 0, "y": 0, "width": 793.701, "height": 1122.52}},
 "items": [
  {"type": "stroke", "t": 1, "layer": "user", "style": {"width": 2.0, "color": "#1e1e1e", "pressureCurve": "linear"}, "points": [12.5, 40.25, 0.43, 13.0, 41.5, 0.51]},
  {"type": "textbox", "t": 2, "layer": "user", "rect": [100, 200, 400, 60], "markdown": "See [[Lecture 3]] #exam"}
 ]}
```

### Coordinates and units

- **Units**: CSS px at 96 dpi, identical to Rnote, so coordinates pass
  through `.rnote` untouched. A4 is 793.701 x 1122.52.
- **Precision**: numbers are written with two decimals.
- **Pages**: pages are not objects. One coordinate space holds everything,
  and page `n` covers `y` from `n * height` to `(n + 1) * height`.
- **Layouts**: `fixed`, `continuousVertical` (the default),
  `semiInfinite` and `infinite`.

### Items

**Common keys**: `type`, `t` (creation order, strictly increasing),
`layer`. The layers are `document`, `image`, `highlighter` and `user`. Paint
order is ascending `(layer, t)`, as in Rnote.

**Item types:**

| Type | Keys |
|---|---|
| `stroke` | `points` (flat `[x, y, pressure, ...]`) and `style`, where `style` is `{width, color, pressureCurve}`. `pressureCurve` is one of `const`, `linear`, `sqrt`, `cbrt`, `pow2`, `pow3`. |
| `shape` | `kind` (`line`, `arrow`, `rect`, `ellipse`, `polyline`, `polygon`), `points`, `style` |
| `textbox` | `rect` `[x, y, w, h]`, `markdown` (source text) |
| `image` | `rect` and `path`, a vault-relative file. Never embedded pixels. |
| `group` | `items` (a list of `t`) plus a payload. Later specs use it for markup anchors, sections and recognition results. |

**Sketch sections (Spec 3, reserved now)**: a companion file holds several
sections as `{"type": "section", "id": "s1", ...}` groups addressed by the
embed fragment `#s1`. Version 1 readers keep them by pass-through.

### Reading and writing

- **Compatibility.**
  - Unknown keys and unknown item types are kept on load and written back
    unchanged on save.
  - A reader seeing `1f 8b` inflates first.
  - Version above 1: opened read-only, with a notice.
- **Writing.**
  - Writes go through `Vault::write`.
  - The parser is `QJsonDocument`, already linked.
  - Parsing happens off the UI thread when the file is over 256 KB.

**Size**: about 15 bytes per point. A heavy note of 100 000 points is about
1.5 MB.

The format is documented for users in `docs/INK.md`, written as part of this
spec's implementation.

## 6. Editor and index seams

These are generic and carry no ink types. The glue passes `std::function`
objects and small abstract interfaces, as `setSpelling` does
(MarkdownEditor.h:220-228). Spec 1 builds seams 1, 2 and 6, which the
handwritten-note tab and the module host need. Seams 3, 4 and 5 are
specified here so Specs 3 and 4 fit, and are built in those specs.

| Seam | Built in | What it does |
|---|---|---|
| 1. View factories by suffix | Spec 1 | `ModuleHost::registerView(suffixes, factory)`. `MainWindow::isViewFile` (MainWindow.cpp:6275) and `openView` (MainWindow.cpp:6297) consult the map after the built-in kinds, gated by `featureOn(id)`. This is the only change to the routing. Wikilink resolution (MainWindow.cpp:3891, ~1288) picks up the new suffixes through `isViewFile`. |
| 2. Tablet routing in the editor | Spec 3 (declared in 1) | A `tabletEvent` path in `viewportEvent` (MarkdownEditor.h:1091). If a block view or the overlay claims the point, the event is forwarded and accepted. Otherwise it is ignored, so Qt synthesises the mouse event exactly as today. |
| 3. Block view object | Spec 3 | `setFileEmbedView(suffixes, factory)` beside `setFileEmbedRenderer` (MarkdownEditor.h:527). The object paints into the editor's painter, takes events over its rectangle, reports a height for a width, and calls back with a changed rectangle. |
| 4. Overlay and text geometry | Spec 4 | A painted overlay after `paintBlocks` (MarkdownEditor.cpp:4538). Three geometry calls become public: `offsetAt` (h:1938), a new `rectsForRange`, and per-character rectangles. |
| 5. Wrap a range | Spec 4 | A public `wrapRange(Range, marker)`. It never toggles (unlike `toggleWrap`, EditingCommands.cpp:951-985), pushes one undo step and works in Reading mode. |
| 6. Foreign-file index reader | Spec 1 | `Indexer::setForeignReader(suffixes, reader)`, modelled on `setNotebookReader` (Indexer.h:66). It returns body text, links and tags for `.ink`, which feed search, backlinks, the graph and the quick switcher. |

Seam 2 is declared in Spec 1, because the module host struct carries it, but
it is wired only in Spec 3.

## 7. The ink core (libnotolith-inkcore)

### Model

`InkDocument` holds the document settings and the items. Strokes are
`shared_ptr<const Stroke>`, immutable once finished, which makes undo
snapshots cheap. A uniform grid of item bounding boxes answers viewport,
eraser and lasso queries.

### Input

One state machine is shared by every surface. It is fed down, move and up
events carrying position, pressure, timestamp, pointer type and buttons.

- **Sources:**
  - **Pen**: `QTabletEvent` accepted on the canvas widget, with
    `setTabletTracking(true)` for hover. Pointer type `Eraser` erases. The
    middle button (BTN_STYLUS) is a held eraser and the right button
    (BTN_STYLUS2) a held lasso.
  - **Touch**: `QTouchEvent` with `WA_AcceptTouchEvents`, used for pan and
    pinch only.
  - **Mouse**: strokes at pressure 0.5, only while the "Draw with mouse"
    toggle is on. The toggle is on the toolbar and is off by default.
- **Mouse with the toggle off**, in the handwritten-note tab:
  - wheel and drag scroll;
  - a click on a text box edits it;
  - Ctrl+drag lasso-selects.

### Smoothing

- **Library**: ink-stroke-modeler, pinned to a commit with FetchContent and
  linked as its `InkStrokeModeler::stroke_modeler` target. Abseil comes from
  the system `find_package(absl)`, with a pinned FetchContent fallback for
  Android and other systems without it. Apache-2.0 is GPL-3.0 compatible.
- **Parameters**: Rnote's overrides of `ModelerParams::suggested()`, in the
  app's units (px, seconds).
- **Prediction**: the predicted tail at the stroke end is drawn transiently
  and never stored.
- **Raw input**: a "Raw pen input" setting, off by default.

### Rendering

**Strokes.** One closed outline per stroke:
- the four offset points per segment at `width * curve(pressure)`;
- round caps;
- all segments in one `QPainterPath`, filled once with `Qt::WindingFill`.
This is Rnote's `compose_lines_variable_width`, ported with attribution.

**Tile cache.** Finished ink is cached in 512 px `QImage` tiles:
- tiles are rasterised on the thread pool;
- the cache key is zoom bucket, device pixel ratio and tile revision;
- a finished, erased or moved item invalidates only the tiles it touches.

**Live stroke.** Only its newest segment is rasterised, into an overlay
image, and `update(segmentRect)` repaints just that segment.

**Zoom.** During a pinch the tiles are scaled. They re-rasterise 400 ms
after the gesture settles, off-thread.

**Backgrounds.** One pattern tile per period via `drawTiledPixmap`. The
seven page styles of Jernej's Rnote fork are ported with attribution.

### Tools in Spec 1

| Tool | Behaviour |
|---|---|
| Pen | Width and colour from the toolbar. |
| Highlighter | Not in Spec 1. Its only effect is on text (applying `==` to covered characters, in Markdown notes and in text boxes), and over handwriting or an empty page it does nothing. So it arrives with the text geometry in Spec 4. The `highlighter` layer exists in the model only so that `.rnote` highlighter strokes keep their paint order. |
| Eraser | Two modes, as in Rnote: stroke (removes the whole stroke) and split (cuts strokes where it passes). |
| Lasso and rectangle select | Move, resize, delete, copy and paste within the document. |
| Shapes | Line, arrow, rectangle, ellipse. |
| Text box | A tap with the text tool creates one. Editing opens a child `MarkdownEditor` over its rectangle, the `CanvasView::beginEditingText` pattern (CanvasView.cpp:231-331). At rest the box renders through `makeContentEditor(text, size)->grab()` (CanvasRender.cpp:377-396) into the tile cache. |

### Undo and redo

- **Stacks.** Each document has an undo stack and a redo stack of
  document snapshots (shared stroke pointers, so a snapshot is small),
  capped at 100.
- **When a snapshot is taken.** On pen-up, an erase, a lasso commit, a
  shape commit and a text-box commit.
- **Redo.** Cleared by any new edit.
- **Keys.** Ctrl+Z undoes, and both Ctrl+Shift+Z and Ctrl+Y redo. They
  belong to the focused surface.

## 8. The handwritten-note tab (InkView, part of the ink module)

- **The view.** `.ink` opens through seam 1 as `InkView`, a
  `QAbstractScrollArea` holding `InkCanvas`, shaped like `PdfView`.
- **Toolbar.**
  - tools: pen, eraser, lasso, shapes, text box;
  - colour and width;
  - "Draw with mouse";
  - undo and redo;
  - zoom;
  - page settings (layout, format, background, pattern).
- **Autosave.** Through the existing `modificationChanged` and
  `armAutosave` route (MainWindow.cpp:6320-6330).
- **Pages.**
  - Page borders are drawn, with a gap and shadow between pages.
  - `continuousVertical` grows the extent by one page when ink crosses
    the last page's bottom.
  - `fixed` adds pages only through "Add page".
- **New notes.** The command "New handwritten note" creates
  `<folder>/<name>.ink` with the default layout from settings.
- **Index.** Seam 6 extracts every text box's Markdown, so `[[links]]` and
  `#tags` written in text boxes show up in backlinks, the graph and search.

## 9. Spec 1b: glyph studio (hwfont module shell)

- **The module.** It is a separate module and package (notolith-hwfont).
  It links `libnotolith-inkcore.so` only.
- **Opening it.** The command "Handwriting font studio" opens a studio
  tab.
- **The tab.**
  - the target glyph and sample number (for example "a, sample 2");
  - one ruled cell with baseline, x-height and ascender guides;
  - the pen.
- **Saving.** Enter, the pen's barrel button, or the toolbar tick saves the
  sample and moves to the next target. Backspace discards. Undo and redo
  work inside the cell.
- **Glyph list.**
  - shows the sample count per glyph;
  - sorts "needs more" first;
  - a click retargets the studio.
- **Glyph set.** ASCII letters and digits, Latin-1 punctuation and
  `čšžČŠŽ`.
- **Sample targets.** 3 samples per letter and 1 per punctuation mark, set
  in Settings.
- **Storage.** `Handwriting/<font name>/<glyph>/<n>.ink` in the vault.
  Glyph folder names are the code point in hex (`0061`), so case and
  diacritics never clash on any filesystem. Each sample is a one-page
  `.ink`, its document carrying a `glyph` object
  `{codepoint, baseline, xHeight, ascender}` (a key the v1 format
  pass-through already tolerates).
- **Not in 1b.** Training, variants, approval and the font build belong to
  Spec 7.

## 10. Milestone A0

A0 is the smallest slice that unblocks Spec 1b. After it, the rest of Spec 1
and Spec 1b proceed in separate worktrees and build directories.

A0 contains:
- the `Stroke` and `InkDocument` model;
- the input state machine, with the modeler;
- the outline composer;
- a single-page `InkCanvas` with pen, eraser, undo and redo;
- the `.ink` reader and writer;
- the module mechanism (section 4), with seam 1.

A0 leaves out layouts, the tile cache, lasso, shapes and text boxes.

## 11. Pen spike (before implementation)

`tools/inkspike`, about 150 lines.
- **What it is.** A Qt window that logs, per tablet event, the device type,
  pointer type, capabilities, pressure, tilt, buttons, timestamp, and the
  event rate per second. It also has a scribble area.
- **Who runs it.** Jernej, by hand, on PADD (FydeTab, GNOME/mutter) and
  shuttlepod-1 (Wacom, Hyprland). runabout is deferred, since it is in
  storage.
- **Where the results go.** The session NOTES.md.
- **What it settles before A0 code is written:**
  - whether the eraser end is detected;
  - the barrel-button mapping;
  - the real event rate;
  - whether fractional scaling offsets the pen position.

## 12. Testing

The house rules apply:
- tests run through ctest only;
- builds and worktrees go in `/tmp`, under
  `systemd-run --user --scope -p MemoryMax=8G`;
- window tests run offscreen or headless;
- tests never touch the real keyring, config or home.

### Unit tests (`tests/test_ink.cpp`)

- **Model and history**: the model, undo and redo, and redo cleared by a
  new edit.
- **Format**:
  - `.ink` round trip;
  - pass-through of an unknown key and an unknown item type;
  - a gzip-wrapped file read;
  - a version 2 file opening read-only.
- **Drawing**: the outline composer against golden images, at two zoom
  levels and two device pixel ratios.
- **Editing**: the eraser in stroke and split modes; a lasso move and a
  resize.
- **Pages**: `continuousVertical` growth, and `fixed` adding pages.
- **Index**: seam 6 extraction of text-box links and tags.

### Module tests (`tests/test_modules.cpp`)

- A fake module built by the test is discovered without being loaded.
- It is loaded on first open of its suffix.
- A mismatched ABI tag is refused.
- A module whose dependency is missing is reported and not loaded.
- A build with `NOTEAPPC_WITH_INK=OFF` passes the whole existing suite.

### Window tests

- `.ink` opens in `InkView` only when the module is installed and enabled.
- Disabling the feature unregisters the suffix.
- Studio: saving a sample writes `Handwriting/<font>/0061/1.ink`, and the
  glyph list updates.

### Performance gates

Measured in `tools/bench` with and without the module directory, as ratios
(PERFORMANCE.md rule 3):

| Gate | Target |
|---|---|
| Cold start to first paint, installed and unused | Equal to not installed, within noise. The scan is measured separately, after first paint. |
| Keystroke on the 1 MB note | Unchanged. |
| Pen event handling at 200 Hz (synthetic feed) | Under 1 ms of CPU per event. |
| Scrolled frame of a 10 000-stroke page, from tiles | Under 4 ms. |
| Five times the strokes | About five times the tile work, not more. |
| Zoom settle, viewport re-rasterised | Under 100 ms, off-thread. |

Results go in docs/PERFORMANCE.md, with a new row and a fix entry if
anything needed changing.

## 13. Out of scope (YAGNI)

Out of scope everywhere:
- tilt and rotation;
- textured and rough brushes (kept by `.rnote` pass-through, not editable);
- in-app palm rejection, which libinput and the palm-filter daemon own;
- fitting text boxes to ruled lines;
- `.xopp` interchange;
- typing text rendered in the user's handwriting;
- Tier 2 ABI changes for ink.

Out of Spec 1, built in later specs:
- `.rnote` (Spec 2);
- sketch sections, seams 2 and 3 (Spec 3);
- markup over text, seams 4 and 5 (Spec 4);
- moving PDF, image and EPUB onto the ink core (Spec 5);
- recognition (Spec 6);
- font training and build (Spec 7).

Generating letter variants for the font is not stroke synthesis in the
sense cut above. It stays, in Spec 7.

## 14. Risks

- **The cost-when-absent claim depends on `QPluginLoader::metaData()` not
  loading the library.** The first module test and bench verify it before
  anything else builds on it.
- **Pen behaviour differs per compositor.** The spike decides the mapping
  before code depends on it. The test matrix is GNOME on PADD and Hyprland
  on shuttlepod-1.
- **ink-stroke-modeler pins Abseil 20250512.** The system has
  20260817. If the system Abseil does not build it, the FetchContent pin is
  used for desktop too.
- **SDT's coverage of Latin letters and `čšž` is unverified.** This does not
  affect this spec. Spec 7 tests it on real studio samples before
  committing to it.
