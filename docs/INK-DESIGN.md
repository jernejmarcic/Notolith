# Ink core and loadable modules: design

Design reference, with the results of the pen input test (section 10) folded in.

Notolith gets handwriting: pen input, whole handwritten notes, sketch sections
inside Markdown notes, pen markup over Markdown text, PDF, images and EPUB,
local handwriting recognition, and a handwriting font made from the user's own
writing. This document covers what is built:

- the loadable-module mechanism, the editor seams, the `.ink` format and the
  ink core, with a basic handwritten-note tab;
- the glyph studio.

The later pieces (sketch sections, markup over text, ink on PDF,
images and EPUB, recognition, font training) are not built. The decisions they
inherit are fixed in section 2, so nothing here has to be re-decided later.

## 1. Intent

The requirements:

- Two equal uses: fully handwritten notes, and handwritten sections inside
  normal Markdown notes. Plus circling and scribbling on Markdown, PDF "and
  such".
- An own open format.
- Built "block by block": "a user can have a hyper minimal, ultra fast
  workflow that has nothing but text". Every non-essential feature is a
  plugin that "shouldn't even need to be installed".
- The handwriting font is a separate plugin. It is "kind of trained", with
  several variants of each letter so the text looks handwritten. A glyph
  studio, where you write a letter and it is saved, comes early so it can
  be tested early.

Success criteria:

1. **Without the ink packages**, the app behaves and benchmarks exactly as
   today.
2. **Installed but unused**: cold start to first paint, and a keystroke on
   the 1 MB note, are unchanged within noise.
3. **Pen to pixel**: well inside one frame. Handling one pen event costs
   under 1 ms of CPU at 200 Hz on a Radeon 780M 2-in-1 laptop and on a FydeTab Duo (RK3588S).
4. **Large pages**: a page of 10 000 strokes scrolls at the display rate.
5. **Open format**: `.ink` is plain and documented. A second program could
   read it from section 5 alone.

## 2. Decisions for all ink work

Later work builds on these decisions without re-asking.

### Input

| Topic | Decision |
|---|---|
| Pen | Draws. The eraser end erases. The lower barrel button is a held eraser and the upper one a held lasso These are a bonus, not the way in: some pens (e.g. the FydeTab Duo's) have no eraser end and no working buttons (pen input test, section 10), so every tool (pen, eraser, lasso, shapes, text, and the highlighter when it arrives) is reachable from the toolbar and by a keyboard shortcut with no pen hardware at all. |
| Pressure | Normalised per device by a learned maximum (setting `ink.pressureMax`, default 1.0, stored per device name): one pen (the FydeTab Duo's) never reports above 0.657, so raw pressure would make every line thin. |
| Touch | Scrolls and pinch-zooms. It never draws. |
| Mouse | Draws only while mouse inking is explicitly on. Otherwise it behaves as it does now, inside ink areas too. |
| Smoothing | ink-stroke-modeler is a required dependency of the ink module, and smoothing is on by default. Raw input is a setting at most. |
| Undo | Every ink surface has an undo stack and a redo stack. Redo is cleared by a new edit. |

### Files and storage

| Topic | Decision |
|---|---|
| `.ink` format | Plain JSON, written by `QJsonDocument::toJson(Compact)` with no custom emitter. The reader sniffs the gzip magic bytes (`1f 8b`) from v1 and inflates such a file (read is supported, in the editor and in the host's index reader); the writer always produces plain JSON. |
| Paper | Chosen per note from four layouts. The default is A4 pages stacked downward, with a page added as the ink grows. |
| Typed text box | It is the real Markdown editor (canvas text-card pattern), so links, tags and formatting work in it. |
| Search | Text boxes are indexed now. Recognised handwriting is indexed later, with recognition. |
| Companion files | One hidden companion file per host file under a tidy folder, `Sketches/` by default, mirroring the vault path:<br>- `Uni/Networks/Lecture 3.md` uses `Sketches/Uni/Networks/Lecture 3.ink`.<br>- `paper.pdf` uses `Sketches/paper.pdf.ink`.<br>It holds the sketch sections and the markup. It follows the host file on rename, move and delete. |
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
| Recognition | Local only, and confirm before creating a link: nothing is created from recognised handwriting without a confirmation. The engine is chosen by a benchmark on real handwriting samples; PP-OCRv6 is a candidate and Tesseract the fallback, neither is a decision. Results are stored inside the `.ink` file. Hand-written `[[link]]`, `#tag`, headings and checkboxes are detected. The model weights are an optional data download, not a module. |
| Font | Version 1 is captured studio samples: several variants per letter, cycled with an OpenType `calt` feature, built with fontTools. A style model (SDT, DeepVecFont v2) is added only if v1 is judged not hand-written enough. Every glyph is approved by eye. This default may be overruled. |
| Glyph samples | Stored in the vault under `Handwriting/<font>/`, so they can be synced to a training machine. |

### Delivery

| Topic | Decision |
|---|---|
| Order | The core and the glyph studio first. Then sketch sections, markup over text, PDF, image and EPUB ink, recognition, and font training and build. |

## 3. Blocks and install units

Each block lives under `libs/ext/<name>` and has its own CMake option and
its own runtime feature id. There are two install units (packages) in this
design: `notolith-ink` (the ink core library plus the ink module, and later
the markup and recognition hooks) and `notolith-hwfont` (the font
module, which depends on `notolith-ink` for the core library). Only the ink core, the ink module and the glyph
studio are built; the rest of the table fixes names so the seams fit them.

| Block | Directory | Install unit | CMake option | Feature id | Built |
|---|---|---|---|---|---|
| ink core library | libs/ext/ink/core | `libnotolith-inkcore.so`, package notolith-ink | `NOTEAPPC_WITH_INK` | - | yes |
| ink module (handwritten-note tab, later sketch sections) | libs/ext/ink | `notolith-ink.so`, package notolith-ink | `NOTEAPPC_WITH_INK` | `ink` | yes (sketch sections later) |
| markup | libs/ext/inkmarkup | `notolith-ink-markup.so`, package notolith-ink | `NOTEAPPC_WITH_INK_MARKUP` | `ink-markup` | no |
| recognition | libs/ext/hwr + tools/hwr_worker | `notolith-hwr.so` + worker, package notolith-ink | `NOTEAPPC_WITH_HWR` | `handwriting-recognition` | no |
| handwriting font | libs/ext/hwfont + tools/hwfont_worker | `notolith-hwfont.so` + worker, package notolith-hwfont | `NOTEAPPC_WITH_HWFONT` | `handwriting-font` | studio only |

Dependencies:
- **One direction only.** Modules link `libnotolith-inkcore.so`. Nothing in
  `libs/editor`, `libs/index` or `libs/ui` links any ink library.
- **Finding the core library.** Modules use RPATH `$ORIGIN`.
- **pdfview, imageview and epub** reach ink at runtime through the module
  host, never by linking. The base binary must load without the package.
- **Package dependencies belong to the package manager.** `notolith-hwfont`
  depends on `notolith-ink`. The manifest has no dependency list and the
  registry has no dependency check. A module whose core library is missing
  fails to load with the loader's own error string
  (`QLibrary::ResolveAllSymbolsHint` makes the failure immediate), and
  Settings reports that string.
- **Abseil is required.** The core library needs the system Abseil
  (`find_package(absl)`). When it is absent the ink core is not built: the
  option is turned off with a configure message.

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
  - an ABI tag.
- Discovery reads it with `QPluginLoader::metaData()`, which does not load
  the library.

### Discovery and loading

1. **Directories.** `<libdir>/notolith/modules/` only. The environment
   variable `NOTEAPPC_MODULE_DIRS` replaces it (used by tests; only in `-DNOTEAPPC_DEV_ENV_OVERRIDES=ON` builds). A vault
   switch does not rescan: the scan is once per window (the registry is a `MainWindow` member), by design. On Android
   the modules are linked in as Qt static plugins (`Q_IMPORT_PLUGIN`) and
   the registry also lists `QPluginLoader::staticPlugins()`.
2. **When.** The scan runs on the thread pool after first paint
   (PERFORMANCE.md rule 5). It costs a directory listing plus one metadata
   read per module.
3. **What it registers.** The suffixes and commands from each manifest.
4. **Loading.** `QPluginLoader::instance()` loads a module only on first
   use: a file of its suffix opens, one of its commands runs, or one of its
   embeds scrolls into view. Nothing else counts as use. In particular
   indexing never loads a module: `.ink` files are indexed by a host-side
   reader (seam 6) that parses the JSON itself.
5. **Suffix claims.** A built-in kind owns its suffixes. Among modules, the
   first one to claim a suffix wins; a later claimant is refused with a
   problem string, in the registry and in `registerView` alike.
6. **Problems.** A manifest with no id, a library that is not a Notolith
   module, an ABI mismatch, a suffix claim refused and a load failure each
   produce a one-line problem, listed in Settings. Nothing is dropped
   silently.

**Gate.** The first bench must verify that the scan does not load the
libraries.

### ABI and settings

- **ABI tag**: same build only, formed as
  `PROJECT_VERSION + "-" + a random token + "-" + Qt6_VERSION`. The token is
  generated once per CMake build directory (`string(RANDOM)`, cached as
  `NOTEAPPC_ABI_TOKEN`), so two builds of the same commit in different
  directories never mix, and a git hash (absent in a tarball build) is not
  needed. One configured header feeds both `ModuleRegistry` and the CMake
  module manifests. A mismatch is refused with a notice naming the module.
  A stable C ABI stays Tier 2's job.
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
- the editor pays one registry lookup per file open and per wikilink
  resolution, and nothing per paint or per viewport event (the per-paint
  pointer checks belong to seams 2 and 3, which the sketch-section work builds).

**Follow-up, out of scope:** canvas, bases, pdfview, imageview, epub, graph
and publish could move to the same mechanism and shrink the base binary.
That is a separate decision.

**Packaging.** The Arch PKGBUILD grows split packages: `notolith`,
`notolith-ink` (core library and ink module) and `notolith-hwfont`, which
depends on `notolith-ink`. The plain CMake install puts modules in
`<libdir>/notolith/modules/`. A module object is shared by every window of
a process and `attach()` runs once per registry, so a real module is
stateless or keeps its state per host.

**Executable cost.** Modules need host symbols, so the base binary links with
`ENABLE_EXPORTS`. Cold start and executable size are measured
against `main`. If cold start regresses by more than 5 % or size by more than
10 %, the fix is `-fvisibility=hidden` plus an export macro on the host
classes modules use. Measure first; nothing changes before that.

## 5. The `.ink` format, version 1

UTF-8 JSON, one top-level object, written compact by
`QJsonDocument::toJson(Compact)` with no hand-rolled emitter. Key order is
Qt's (sorted), so a re-save of an unchanged document is byte-identical. The
example below is shown spaced out for reading.

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

- **Units**: CSS px at 96 dpi. A4 is 793.701 x 1122.52.
- **Precision**: numbers are written with three decimals (rounded before
  the document is handed to `QJsonDocument`).
- **Pages**: pages are not objects. One coordinate space holds everything,
  and page `n` covers `y` from `n * height` to `(n + 1) * height`.
- **Layouts**: `fixed`, `continuousVertical` (the default),
  `semiInfinite` and `infinite`.

### Items

**Common keys**: `type`, `t` (creation order, strictly increasing),
`layer`. The layers are `document`, `image`, `highlighter` and `user`. Paint
order is ascending `(layer, t)`.

**Item types:**

| Type | Keys |
|---|---|
| `stroke` | `points` (flat `[x, y, pressure, ...]`) and `style`, where `style` is `{width, color, pressureCurve}`. `pressureCurve` is one of `const`, `linear`, `sqrt`, `cbrt`, `pow2`, `pow3`. |
| `shape` | `kind` (`line`, `arrow`, `rect`, `ellipse`, `polyline`, `polygon`), `points`, `style` |
| `textbox` | `rect` `[x, y, w, h]`, `markdown` (source text) |
| `image` | `rect` and `path`, a vault-relative file. Never embedded pixels. |
| `group` | `items` (a list of `t`) plus a payload. Later work uses it for markup anchors, sections and recognition results. |

**Sketch sections (not built, reserved)**: a companion file holds several
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

**Size**: about 18 bytes per point. A heavy note of 100 000 points is about
1.8 MB.

The format is documented for users in `docs/INK.md`.

## 6. Editor and index seams

These are generic and carry no ink types. The glue passes `std::function`
objects and small abstract interfaces, as `setSpelling` does
(MarkdownEditor.h:220-228). Seams 1 and 6 are built, which the
handwritten-note tab and the module host need; seam 2 is declared. Seams 3, 4
and 5 are specified here so sketch sections and markup over text fit, and are
built with them.

| Seam | Status | What it does |
|---|---|---|
| 1. View factories by suffix | built | `ModuleHost::registerView(suffixes, factory)`. `MainWindow::isViewFile` (MainWindow.cpp:6275) and `openView` (MainWindow.cpp:6297) consult the map after the built-in kinds, gated by `featureOn(id)`. This is the only change to the routing. Wikilink resolution (MainWindow.cpp:3891, ~1288) picks up the new suffixes through `isViewFile`. |
| 2. Tablet routing in the editor | declared, not wired | A `tabletEvent` path in `viewportEvent` (MarkdownEditor.h:1091). If a block view or the overlay claims the point, the event is forwarded and accepted. Otherwise it is ignored, so Qt synthesises the mouse event exactly as today. |
| 3. Block view object | not built | `setFileEmbedView(suffixes, factory)` beside `setFileEmbedRenderer` (MarkdownEditor.h:527). The object paints into the editor's painter, takes events over its rectangle, reports a height for a width, and calls back with a changed rectangle. |
| 4. Overlay and text geometry | not built | A painted overlay after `paintBlocks` (MarkdownEditor.cpp:4538). Three geometry calls become public: `offsetAt` (h:1938), a new `rectsForRange`, and per-character rectangles. |
| 5. Wrap a range | not built | A public `wrapRange(Range, marker)`. It never toggles (unlike `toggleWrap`, EditingCommands.cpp:951-985), pushes one undo step and works in Reading mode. |
| 6. Foreign-file index reader | built | `Indexer::setForeignReader(suffix, reader)`, modelled on `setNotebookReader` (Indexer.h:66). It returns body text, links and tags for `.ink`, which feed search, backlinks, the graph and the quick switcher. The `.ink` reader is host-side: `MainWindow` registers it next to the `.ipynb` one (`updateNotebookIndex()`), parsing the JSON with `QJsonDocument` and returning the markdown of every `textbox` item. It never loads a module and `libs/ui` links no ink library; the key names are duplicated as string literals, with a comment pointing at section 5. The host reader inflates a gzip-compressed `.ink` too (zlib, one member, at most 16 MiB inflated, the same as its plain-file cap); a file over 16 MiB is not indexed. It is registered for every window whether or not the ink module is installed, loaded or switched on (it is never gated on the feature). |

Seam 2 is declared, because the module host struct carries it, but it is wired
only when sketch sections are built.

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
    (BTN_STYLUS2) a held lasso. These are defaults and a bonus: some pens (e.g. the FydeTab Duo's)
    report a tip only, no eraser end and no working buttons, so every
    tool is also reachable from the toolbar and by keyboard shortcut.
  - **Rate and timestamps**: the pen reports at 120 Hz in contact
    (`kInputRateHz = 120`). Equal timestamps are coalesced and backwards
    ones skipped; the pen input test saw 128 equal pairs and no backwards ones.
    The fractional-scale offset stays `{0, 0}` (device pixel ratio 2 showed
    none).
  - **Pressure**: raw pressure is divided by a per-device learned maximum
    (setting `ink.pressureMax`, default 1.0, stored per device name) and
    capped at 1. The maximum is learned from the largest pressure seen; the
    FydeTab Duo pen tops out at 0.657 with a median of 0.21.
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
  the system `find_package(absl)` and is required: without it the ink core is
  not built. There is no bundled copy. Apache-2.0 is GPL-3.0 compatible.
- **Parameters**: overrides of `ModelerParams::suggested()`, in
  the app's units (px, seconds).
- **Prediction**: the predicted tail at the stroke end is drawn transiently
  and never stored.
- **Raw input**: a "Raw pen input" setting, off by default.

### Rendering

**Strokes.** One closed outline per stroke:
- the four offset points per segment at `width * curve(pressure)`;
- round caps;
- all segments in one `QPainterPath`, filled once with `Qt::WindingFill`.
This is Rnote's `compose_lines_variable_width`, ported with attribution (see `docs/INK.md`, Credits).

**Tile cache.** Finished ink is cached in 512 px `QImage` tiles:
- stroke tiles are rasterised on the thread pool (`QImage` with `QPainter`
  is thread-safe); text boxes, and anything that uses `QWidget::grab` or
  `QTextDocument` with widget fonts, render on the UI thread;
- the cache key is zoom bucket, device pixel ratio and tile revision;
- a finished, erased or moved item invalidates only the tiles it touches;
- the cache has a byte budget (setting `ink.tileCacheMB`, default 64) and
  evicts the least recently painted tiles first.

**Live stroke.** Only its newest segment is rasterised, into an overlay
image, and `update(segmentRect)` repaints just that segment.

**Zoom.** During a pinch the tiles are scaled. They re-rasterise 400 ms
after the gesture settles, off-thread.

**Backgrounds.** Ruled lines, grid and dots, drawn as lines for the exposed
rect only (`PagePatterns.cpp`).

### Tools

| Tool | Behaviour |
|---|---|
| Pen | Width and colour from the toolbar. |
| Highlighter | Not built. Its only effect is on text (applying `==` to covered characters, in Markdown notes and in text boxes), and over handwriting or an empty page it does nothing. So it arrives with the text geometry (seam 4). The `highlighter` layer exists in the model so that a file carrying highlighter strokes keeps their paint order. |
| Eraser | Two modes: stroke (removes the whole stroke) and split (cuts strokes where it passes). |
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
- **Index.** The host-side `.ink` reader (seam 6) extracts every text box's
  Markdown, so `[[links]]` and `#tags` written in text boxes show up in
  backlinks, the graph and search, whether or not the module is installed,
  loaded or switched on (the host registers the reader unconditionally).
- **Save contract.** A view opened without a file (the glyph studio) still
  honours `isModified` and `save()`: the tab shows the modified mark, and
  closing or quitting asks, or autosaves, through the same path as a file
  tab.

## 9. Glyph studio (hwfont module shell)

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
- **Not built.** Training, variants, approval and the font build.

## 10. Pen input test

**Results on a FydeTab Duo** (himax-stylus, GNOME/mutter):
- tip only: no eraser end is reported;
- the barrel buttons do not work;
- 120 Hz while in contact;
- pressure maximum 0.657, median 0.21;
- 128 pairs of equal timestamps, none backwards;
- device pixel ratio 2 showed no position offset.

These fixed `kInputRateHz = 120`, the learned pressure maximum, the rule that
every tool is reachable without pen buttons, coalescing of equal timestamps,
and `kFractionalScaleOffset = {0, 0}`. A Wacom pen on Hyprland was not run and is
a follow-up check on the same tool.

`tools/inkspike`, about 150 lines.
- **What it is.** A Qt window that logs, per tablet event, the device type,
  pointer type, capabilities, pressure, tilt, buttons, timestamp, and the
  event rate per second. It also has a scribble area.
- **Who runs it.** By hand, on a FydeTab Duo (GNOME/mutter) and a Wacom
  2-in-1 (Hyprland).
- **What it settled before the ink code was written:**
  - whether the eraser end is detected;
  - the barrel-button mapping;
  - the real event rate;
  - whether fractional scaling offsets the pen position.

## 11. Testing

Tests run through ctest only. Window tests run offscreen or headless, and
tests never touch the real keyring, config or home.

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
- It is loaded on first open of its suffix. The gate test that proves the
  scan does not load the library runs as its own ctest entry, so no other
  test can have mapped the fake first.
- A mismatched ABI tag is refused.
- A second module claiming an already claimed suffix is refused.
- A manifest with no id, and a library that is not a Notolith module, are
  each reported with a problem string.
- A module whose core library cannot be resolved fails to load, and the
  loader's error string is reported.
- Every test runs with `NOTEAPPC_MODULE_DIRS` pointing at an empty directory
  unless it sets its own.
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

Results go in docs/PERFORMANCE.md.

## 12. Out of scope (YAGNI)

Out of scope everywhere:
- tilt and rotation;
- textured and rough brushes;
- in-app palm rejection, which libinput and the palm-filter daemon own;
- fitting text boxes to ruled lines;
- `.xopp` interchange;
- typing text rendered in the user's handwriting;
- Tier 2 ABI changes for ink.

Not built yet:
- sketch sections, seams 2 and 3. They must also hide the
  `Sketches/` companion files from the file tree, the quick switcher and
  search, and rewrite the embed lines when a note is renamed;
- markup over text, seams 4 and 5. It must test a mid-word `==`
  in the parser and against Obsidian's behaviour;
- moving PDF, image and EPUB onto the ink core;
- recognition, whose engine is chosen by a benchmark on
  real handwriting samples;
- font training and build.

Generating letter variants for the font is not stroke synthesis in the
sense cut above. It stays, with font training.

## 13. Risks

- **The cost-when-absent claim depends on `QPluginLoader::metaData()` not
  loading the library.** The first module test and bench verify it before
  anything else builds on it.
- **Pen behaviour differs per compositor.** The pen input test decides the
  mapping before code depends on it. The test matrix is GNOME on a FydeTab
  Duo and Hyprland on a Wacom 2-in-1.
- **ink-stroke-modeler pins Abseil 20250512.** System Abseil is required; if
  the system's version does not build the modeler, the ink core stays off
  until the modeler pin is moved. There is no bundled Abseil.
- **SDT's coverage of Latin letters and `čšž` is unverified.** This does not
  affect this design, and font v1 does not use SDT. Font training tests it on real
  studio samples only if v1 is judged not hand-written enough.
