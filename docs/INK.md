# Handwritten notes (ink)

Notolith opens `.ink` files as pages you write and draw on with a pen: pressure-sensitive strokes, an eraser,
shapes, a selection you can move and resize, typed text boxes that are real Markdown, undo and autosave. The
design is in `docs/INK-DESIGN.md`; this file is what a user, a packager and the next
person to touch the code need to know.

The handwriting code is a pair of **loadable modules**, not part of the base program. Switched off, or not installed,
Notolith never loads it and pays nothing for it (`docs/PERFORMANCE.md`, "Handwriting"). The base program only knows one
thing about `.ink` files: how to read their text boxes for the index (see "Notes in the vault").

## Contents

- Installing and building
- Modules
- The `.ink` format
- Notes in the vault: creating, linking, searching
- Tools, keys and the pen
- Layouts and patterns
- Settings
- Pen pressure
- Performance notes
- Not verified offscreen
- Credits
- Glyph studio

## Installing and building

On Arch the build produces four packages from one `packaging/arch/PKGBUILD`:

| package | holds | depends on |
|---|---|---|
| `notolith` | the application, no handwriting | the usual Qt and library set |
| `notolith-ink` | `libnotolith-inkcore.so` (the ink core library) and `notolith-ink.so` (the handwritten-note module) | `notolith`, `abseil-cpp` |
| `notolith-hwfont` | `notolith-hwfont.so` (the glyph studio, see the last section) | `notolith-ink` |
| `notolith-pdfview` | `notolith-pdfview.so` (the PDF viewer tab) and `libnotolith-pdfpage.so` (poppler's page count, size and render, which PDF embeds in a note use on their own) | `notolith`, `poppler-qt6` |

There are exactly these three module packages (`notolith-pdfview` has no handwriting in it; it is listed because it installs the same way). `notolith-ink` is one install unit: later work (markup,
recognition) adds its hooks to this module, not to new packages. Handwriting recognition model weights, when they
arrive, are an optional data download and not a module. Pacman enforces the dependency of `notolith-hwfont` on
`notolith-ink`: a module manifest has no `dependencies` key.

Install the packages of one `makepkg` run together (`pacman -U` with every file). The module packages
depend on exactly the same version of their parent (`notolith=<pkgver>-<pkgrel>`, `notolith-ink=<pkgver>-<pkgrel>`),
so pacman refuses a mismatched set; a rebuild with an unchanged `pkgrel` is not caught, which is why one run
matters: see "The ABI tag message" below.

**Abseil is a hard requirement of the ink core, and it is the system one.** CMake looks for `find_package(absl)`; if it
is not there the ink core and both modules are simply not built, and the configure step says `Abseil not found: the ink
core and the handwritten-note module are not built (install abseil-cpp)`. There is no bundled copy. On Arch the
package is `abseil-cpp`. The CMake options are `NOTEAPPC_WITH_INK` (the core library and the handwritten-note module,
on by default, never on Android) and `NOTEAPPC_WITH_HWFONT` (the glyph studio, on by default, needs the ink core).

**The build needs the network.** The stroke smoothing library `ink-stroke-modeler` (Google, Apache-2.0) is fetched
with CMake `FetchContent` at configure time and pinned to one commit (`f2388813b0b25bc3e33d143d369a8367ab2e30c8`,
in `libs/ext/ink/core/CMakeLists.txt`). A PKGBUILD cannot pin that without a source tarball, so `source=()` stays
empty and the build fetches it; a later change can vendor it. It is linked statically into
`libnotolith-inkcore.so`, so nothing of it is installed on its own.

## Modules

A module is a shared library with an embedded manifest (`id`, `feature`, file suffixes, commands, an ABI tag). Notolith
scans the module directory on a thread-pool thread after the first window is painted, reading only the manifests, and
loads a module the first time it is used: a file of its suffix opens, or one of its commands runs. Indexing never loads
one.

- **Where modules live.** `<libdir>/noteAppC/modules/`, that is `/usr/lib/noteAppC/modules/` on Arch. It holds
  `libnotolith-inkcore.so`, `notolith-ink.so` and `notolith-hwfont.so`; the two modules find the core library next to
  themselves (RPATH `$ORIGIN`).
- **`NOTEAPPC_MODULE_DIRS`.** Only in a build configured with `-DNOTEAPPC_DEV_ENV_OVERRIDES=ON` (the default for test builds, off in release): when set (colon-separated directories), it replaces that
  directory. The tests and the benchmark use it; it is also the way to try a build tree: point it at `<build>/modules`.
  A vault switch does not rescan: each window scans once, after its first paint (a module library is loaded once
  per process, by whichever window uses it first).
- **Switches.** Settings > Core plugins has one switch per installed module: "Handwritten notes" (`plugins.ink`) and
  "Handwriting font studio" (`plugins.handwritingFont`), both on by default because installing the package is the
  opt-in. With "Handwritten notes" off, `.ink` files are not openable in Notolith (they are attachments), and the
  command "New handwritten note" is greyed.
- **Rows.** A module's own settings rows (below) appear under the same page, after the module has loaded for the first
  time in a session: open a `.ink` file, or run "New handwritten note". The rows of a module that has not loaded yet
  do not exist yet.
- **Commands.** "New handwritten note" (`ink:new`) and "Open glyph studio" (`handwriting-font:studio`) are in the command
  palette while their module is installed and on.
- **A suffix has one owner.** A module cannot claim a built-in file type. If two modules claim one suffix, the first by
  scan order (directories as given, files by name) wins and the other is listed with a problem.

### The ABI tag message

Every module carries an ABI tag, `<version>-<build token>-<Qt version>`, and Notolith refuses a module whose tag differs
from its own. The build token is random, made once per build directory, so two builds of one commit never share a tag.
The refusal shows up as a line in Settings > Core plugins (and in the log):

> `Module ink was built for another Notolith (<its tag>; this one is <the application's tag>) and was not loaded.`

What to do: reinstall `notolith-ink` and `notolith-hwfont` from the **same build** as the `notolith` package, that is
rebuild and install all three together. A Qt upgrade changes the tag too, so after one rebuild all three. Nothing is
lost in the meantime; `.ink` files stay as they are.

Other one-line problems in the same place: a manifest with no id, a module that claims a built-in suffix or one
another module already holds, and a load failure, which is the dynamic loader's own message. The classic one: installing
`notolith-hwfont` without `notolith-ink` fails with the loader complaining that `libnotolith-inkcore.so` cannot be
found. The fix is the package, pacman normally prevents it.

## The `.ink` format

This section is the contract that `tests/test_ink.cpp` pins; the code is `libs/ext/ink/core/InkFormat.cpp` and its
header `InkFormat.h`. The host's own index reader (`libs/ui/InkTextBoxes.cpp`) duplicates the keys and values `format`
(`notolith-ink`), `version`, `items`, `type` (`textbox`) and `markdown`; if they change, they change in both. It
does not read `rect`.

**Encoding.** UTF-8 JSON, one top-level object, written by `QJsonDocument::toJson(QJsonDocument::Compact)` and nothing
hand-rolled. Qt writes the keys sorted, so a save of an unchanged document is byte-identical. Numbers are rounded to at
most three decimals before they are handed to Qt (`roundThree`) and Qt writes them in their shortest form. A number
that is not finite is written as `0`, never as `null`. The example is spaced out for reading:

```json
{"format": "notolith-ink", "version": 1,
 "document": {"format": {"width": 793.701, "height": 1122.52, "dpi": 96},
              "background": {"color": "#ffffff", "pattern": "lines",
                             "patternSize": 32, "patternColor": "#c0c8d0"},
              "layout": "continuousVertical",
              "extent": {"x": 0, "y": 0, "width": 793.701, "height": 1122.52}},
 "items": [
  {"type": "stroke", "t": 1, "layer": "user",
   "style": {"width": 2, "color": "#1e1e1e", "pressureCurve": "linear"},
   "points": [12.5, 40.25, 0.43, 13, 41.5, 0.51]},
  {"type": "textbox", "t": 2, "layer": "user", "rect": [100, 200, 400, 60], "markdown": "See [[Lecture 3]] #exam"}
 ]}
```

**Units.** CSS px at 96 dpi. A4 is 793.701 x 1122.52.
Pages are not objects: one coordinate space holds everything and, in the paged layouts, page `n` covers `y` from
`n * height` (plus the extent's `y`) to `(n + 1) * height`.

**`document`.**

| key | meaning |
|---|---|
| `format` | `width`, `height` (the page, px) and `dpi`. Default A4 at 96. |
| `background` | `color` and `patternColor` (`#rrggbb`, or `#rrggbbaa` when translucent, CSS order), `pattern` (a name from "Layouts and patterns"), `patternSize` |
| `layout` | `fixed`, `continuousVertical` (default), `semiInfinite` or `infinite` |
| `extent` | `x`, `y`, `width`, `height`: the sheet the document currently covers. Grows with the ink. |

`patternSize` is a scalar (a square cell) or a two-number list `[width, height]`. The writer emits a scalar when the two
are equal and the pair otherwise; the reader takes either.

**Items.** Common keys: `type`, `t` (a finite number from 0 up to but not including 9e15, truncated to a whole number, unique in the file:
it is the creation order and the item's identity; the writer only ever produces whole numbers) and `layer` (`document`, `image`, `highlighter` or `user`, default `user`). Paint order is ascending
`(layer, t)`.

| `type` | keys |
|---|---|
| `stroke` | `style` `{width, color, pressureCurve}` and `points`, a flat list of `x, y, pressure` triples. `pressureCurve` is `const`, `linear`, `sqrt`, `cbrt`, `pow2` or `pow3`: the drawn width is `width * curve(pressure)`. |
| `shape` | `kind` (`line`, `arrow`, `rect`, `ellipse`, `polyline`, `polygon`), `style` as above, `points`, a flat list of `x, y` pairs: two for a line, arrow, rect (corners) and ellipse (corners), at least two for a polyline and three for a polygon. No tool draws the last two; a file may carry them. |
| `textbox` | `rect` `[x, y, width, height]` and `markdown`, the box's Markdown source, exactly as typed. |
| `image` | `rect` and `path`, a vault-relative file; never embedded pixels. Drawn as a placeholder: there is no image tool yet. |
| `group` | kept and written back verbatim; later work uses it for markup anchors, sketch sections and recognition results. |

**Reading.**

- A file larger than 64 MiB (`kMaxInflatedBytes`) is refused, plain or gzip, before it is read.
- **Gzip is read.** A file that starts with the gzip magic bytes `1f 8b` is inflated first: one gzip member, nothing
  after it, at most 64 MiB inflated. A damaged, truncated or oversized member is refused with a message. Notolith
  never writes gzip itself; the writer always produces plain JSON.
- `format` must be the string `notolith-ink`.
- **Version policy.** `version` must be a number of at least 1. Exactly `1` opens for editing. Any other number from 1
  up (2, 1.5) is a newer file: it opens **read-only**, with the notice "Read-only: written by a newer Notolith", and
  saving it is refused so nothing newer is rewritten by an older reader. Missing, not a number, or below 1 is refused.
  The writer always stamps `1`. A change that older readers would misread has to raise the version; adding keys does not.
- Refused (the file does not open, the message names the problem): `items` that is not a list, `document` or its
  `format`, `background` or `extent` that is not an object, a stroke whose `points` are not triples or whose `style`
  is not an object, a shape with too few points, a `textbox` or `image` whose `rect` is present but not four numbers,
  two items with one `t`, an item with no usable `t`, a page, extent or dpi that is not a positive finite number, a
  pattern size under 0.001. A wrong-typed scalar (a string where a number goes) takes its default silently.
- **Nothing is dropped.** An item of a type this version does not know, and a `shape` of an unknown `kind`, is kept as it
  was read and written back byte for byte (no key added, none changed), and is not drawn. Unknown keys are kept at every
  level: at the top, in `document`, in `format`, `background` and `extent`, in `style`, and inside known item types; a
  save starts from what was read and writes only the keys the model owns over it. The glyph studio's `glyph` key under
  `document` is one such key.
- Parsing happens off the UI thread when the file is over 256 KiB (`kOffThreadParseBytes`).

**Size.** About 18 bytes a point. A heavy note of 100 000 points is about 1.8 MB.

## Notes in the vault: creating, linking, searching

- **New note.** The command "New handwritten note" creates `Handwritten note.ink` (then `Handwritten note 2.ink`, and
  so on) in the folder of the active note, or the vault root when there is none, takes the layout and pattern from the
  Settings rows below, and opens it.
- **Linking.** A `.ink` file is a note whose name keeps its suffix, like a `.ipynb` notebook: write `[[Sketch.ink]]`, not
  `[[Sketch]]`. `[[Sketch]]` resolves to `Sketch.md` if there is one and never to the ink note. With "Handwritten notes"
  off (or the package missing), a link to a `.ink` file cannot be opened in Notolith; it is an attachment.
- **Searching.** The host indexes the text boxes of every `.ink` file itself, by a small reader of its own that parses
  the JSON (`libs/ui/InkTextBoxes.cpp`, registered with `Indexer::setForeignReader` in `MainWindow::updateNotebookIndex`).
  The text of a note is the Markdown of its text boxes in file order, joined by a blank line, so the `[[links]]` and
  `#tags` in a text box show up in backlinks, tags and search. Handwritten strokes are not text and are not indexed
  (recognition is a later, optional feature).
  **This does not depend on the module or on the switch.** The reader is registered for every window, whether the
  ink module is installed, loaded, or switched off; indexing never loads a module and `libs/index` and `libs/ui` link no
  ink library.
- **Limits of the reader.** Plain JSON or gzip. A file over **16 MiB** (`kInkIndexMaxBytes`), plain or inflated, is not
  indexed at all: it still opens in the editor (whose cap is 64 MiB) but has no search, tag or backlink entries, and
  when a note grows past 16 MiB its index row is removed on the next scan. At most 1 Mi (1 048 576) characters of text
  are taken from one file, cut at a character boundary. A file the editor would refuse (trailing data after the gzip
  member, no valid `version`, not `notolith-ink`) is not indexed either, so nothing shows in search that cannot be
  opened. At most two `.ink` files are parsed at once, to bound memory. A file with no text boxes is indexed as an
  empty note.
- **Rename.** Renaming a note rewrites the `[[links]]` inside text boxes too. Known gap: a text box that holds both a
  `[[wikilink]]` and a Markdown link can be missed by the rewrite.

## Tools, keys and the pen

Every tool is on the toolbar and on a key, so everything works with no pen hardware at all. That matters: the
FydeTab Duo's pen (used for the pen input test) has a tip only, no eraser end and no working barrel buttons. The keys need the
drawing surface focused (click it once).

| tool | key | what it does |
|---|---|---|
| Pen | `P` | Draws. Width, colour (six swatches taken from the page's theme) and pressure curve are on the toolbar. |
| Eraser | `E` | Takes the whole stroke it touches. The toolbar toggle "Erase parts" or `Shift+E` makes it take only the touched segments, the rest of the stroke surviving as one or two strokes. Shapes, text boxes and images are always taken whole. |
| Select | `L` | Lasso, or a rectangle (`Shift+L` switches between them; so does the toolbar's menu). A stroke is selected when any of its points is inside the region or a segment crosses it; a shape when its ink meets it; a text box when the region holds the centre of its rectangle or crosses its edge. Drag the selection to move it, drag one of the eight handles to resize it. |
| Shape | `S` | Line, arrow, rectangle, ellipse by dragging; `Shift+S` goes to the next kind in that order. A drag under 3 widget px commits nothing. |
| Text | `T` | Click to write a text box, or to edit the one under the pointer. The editor is the note editor (Markdown, live preview), so links, tags and formatting work. A new box is 260 x 72 px; resize it with the selection handles (the text reflows, it is not scaled). |

| key | action |
|---|---|
| `Ctrl+Z` | Undo. History keeps the last 100 steps; a new edit clears redo. |
| `Ctrl+Shift+Z`, `Ctrl+Y` | Redo. |
| `Delete` | Delete the selection. |
| `Ctrl+C`, `Ctrl+V` | Copy the selection; paste it at the centre of the visible page, 20 px right and down, as the new selection. |
| `Escape` | In a text box: commit it. During a select or shape drag: cancel the drag and keep the selection; pressed again: clear the selection. |
| `Ctrl` + wheel | Zoom, 10 percent a notch, between 25 and 800 percent (the toolbar's zoom box has 50, 100 and 200). The plain wheel scrolls. |

The toolbar also has the "Draw with mouse" toggle, undo, redo, delete, copy and paste buttons, the pen width box and
the **Page** menu (see "Layouts and patterns"). A text box commits as one history step when you press `Escape`, click
elsewhere, change tool, undo, or the view is hidden or loses focus; an emptied box is deleted, and a new box with no
text is never added.

**The clipboard.** Copy puts the selection on the clipboard as the MIME type `application/x-notolith-ink+json` (a
`{"format":"notolith-ink-clip","items":[...]}` document of the same item JSON as the file), so it pastes into another
ink note. A payload over 16 MiB (`kMaxClipboardBytes`) is refused when pasting (a status line says so), so a copy of
that size cannot be pasted back; there is no cap on the copy side. Plain text does not paste into a note.

**Input devices.**

- **Pen.** Draws, at the pen's pressure. Whatever the tool, the eraser end of a pen erases.
- **Barrel buttons** (defaults from the spec; the names are constants in `libs/ext/ink/core/InkInput.h`):
  the lower button, `Qt::MiddleButton` (`BTN_STYLUS`), **erases while it is held**; the upper button,
  `Qt::RightButton` (`BTN_STYLUS2`), **selects while held** (with the Pen tool, for that one stroke; the selection then
  stays for the Select tool). In the glyph studio, a click of the upper button saves the sample instead (below).
  `kHeldEraserButton`, `kHeldLassoButton` and `kTrustEraserPointerType` ("an eraser end is an eraser") keep these
  defaults because the pen input test measured nothing else: the test pen's buttons were broken and it has no eraser
  end, so the mapping is **not yet verified on a pen that has them** (a Wacom pen is the test). If a
  pen reports other buttons, those constants are the one place to change.
- **Mouse.** With "Draw with the mouse" off (the default) a left drag scrolls the page (a hand cursor), and
  `Ctrl`+drag selects in any tool; the Text tool still takes a click. On, the mouse draws like a pen at pressure 0.5.
  The setting is `ink.drawWithMouse`, and the toolbar toggle is the same setting.
- **Touch.** One finger scrolls, a pinch zooms; touch never draws. While a pen stroke is in flight touch is ignored,
  so a resting palm does not move the page.
- **Smoothing.** Pen strokes go through ink-stroke-modeler with tuned parameters (wobble smoothing, the spring
  position model, a predicted tail). "Raw pen input" (`ink.rawInput`) skips it and keeps every tablet sample.
- Pressure is normalised per pen; see "Pen pressure".

**Read-only.** A canvas opened read-only (a newer file) ignores the pen, the mouse and the edit keys but still scrolls and
zooms.

## Layouts and patterns

Page size is chosen in the **Page** menu: A4, A5, Letter or Custom (width and height in millimetres), plus the page
colour, the layout, the pattern and its size. Each change is one history step. "Add page" adds a page to the paged
layouts.

| layout name | behaviour |
|---|---|
| `fixed` | A stack of pages that never grows by itself; "Add page" adds one. |
| `continuousVertical` | The default: A4 pages stacked downward, with a gap and a shadow between them; a page is added when ink crosses the bottom of the last one. |
| `semiInfinite` | One sheet that grows right and down as the ink reaches its edge (by the ink plus a page of room), never left or up. |
| `infinite` | One sheet that grows in every direction. |

A document is at most 100 000 pages (`kMaxPages`) on a side.

| pattern name | page style |
|---|---|
| `none` | blank |
| `lines` | ruled lines |
| `grid` | a grid |
| `dots` | dots |

The pattern
is drawn straight from the geometry, only for the lines that are on screen, and it is anchored to the page (to the
document origin on a sheet), so a sheet that grows up or left never moves the pattern under the ink. A file that
names any other pattern opens with `lines`.

## Settings

Under Settings > Core plugins, once the module has loaded. All are stored in the application's `settings.ini` like any
other setting.

| key | row | default | range or values |
|---|---|---|---|
| `ink.drawWithMouse` | Draw with the mouse | off | on, off |
| `ink.rawInput` | Raw pen input | off | on, off |
| `ink.defaultLayout` | Page layout of new notes | `continuousVertical` | the four layouts |
| `ink.defaultPattern` | Page pattern of new notes | `lines` | the four patterns |
| `ink.defaultPenWidth` | Pen width | 2.0 | 0.5 to 40, in document px; read when a note opens |
| `ink.eraserWidth` | Eraser width | 20 | 4 to 100, in document px |
| `ink.tileCacheMB` | Handwriting tile cache (MB) | 64 | 8 to 1024; see "Performance notes" |

Hidden keys, with no row, that the view keeps for you:

| key | holds |
|---|---|
| `ink.pressureMax` | the seed maximum for a pen with nothing stored, default 1.0 (see "Pen pressure") |
| `ink.pressureByDevice` | the learned maximum of each pen, a compact JSON object `{"<device name>": <maximum>}` |
| `ink.eraserMode` | `stroke` or `split`: the eraser's last mode (the "Erase parts" toggle) |
| `ink.selectShape` | `lasso` or `rect`: the select tool's last shape |
| `ink.shapeKind` | `line`, `arrow`, `rect` or `ellipse`: the shape tool's last kind |

The glyph studio's keys are `hwfont.fontName`, `hwfont.samplesPerLetter` and `hwfont.samplesPerPunctuation` (below).

A saved note is written through the vault's atomic writer; autosave follows the application's `files.autosave` setting,
as for a Markdown note. If the file changes on disk while it is open (another device's sync, a rename rewriting a link in it),
a tab with nothing unsaved reloads it at once, as a note does; a tab with unsaved edits keeps them, and its next save
keeps your version in a side file and shows the disk's. If the file on disk cannot be read, the next save first copies its bytes to "<name> (unreadable <date time>).ink" beside it, then writes your document. A file that cannot be parsed does not open; the reader's message says why.

## Pen pressure

A pen's raw pressure rarely reaches 1.0: the FydeTab pen never exceeds 0.657 (median 0.21), which would draw every line
thin. Notolith therefore divides each pen's pressure by a **learned maximum**, kept per device name
(`QPointingDevice::name()`) and stored in `ink.pressureByDevice`.

- A pen with nothing stored starts at the seed, `ink.pressureMax` (1.0: no change).
- After 200 samples above 30 percent of the current maximum, the maximum becomes the highest pressure seen. That can
  lower it from 1.0, which is the point. From then on it only rises, when you press harder than ever; it never goes below
  0.1.
- A harder press is saved a moment later (1.5 s after the last change, once for a run of them) and when the tab closes.
- A pressure over 1 is treated as 1.0 and teaches nothing (a bad sample, not a harder press); an unknown pressure (a device with none) becomes 0.5. A mouse
  stroke is always 0.5. Only a pen's pressed samples teach it, not the release.
- To start the learning again, delete the pen's entry from `ink.pressureByDevice` (or the whole key) in `settings.ini`.

## Performance notes

The full account, with the numbers, is in `docs/PERFORMANCE.md` under "Handwriting". What it means for use:

- **Nothing runs until you touch ink.** The module scan runs on the thread pool after the first paint, loads nothing,
  and a window without a `.ink` file open never loads a module. Cold start differs by no measurable amount with the
  module directory present or empty.
- **Painting never renders.** Committed items come from 512 px tiles rendered on the thread pool; a paint shows the tiles
  it has (blurry, or the page colour where there is none) and asks the pool for the rest. The live stroke is rasterised a
  segment at a time into an overlay. A paint also cancels the queued renders for cells that left the screen
  (`TileCache::keepOnly`), so after a fast scroll the visible tiles are not stuck behind tiles nobody wants. After a
  zoom the tiles are scaled until the zoom has been still for 400 ms, then re-rendered sharp off the UI thread.
- **Tile cache.** `ink.tileCacheMB` (default 64, 8 to 1024) bounds the rendered tiles in memory; the least recently
  shown go first, and the tiles the screen needs are never evicted for it. It applies when a note opens and when the
  setting changes.
- **Text boxes** are drawn on the UI thread (they use the note editor, a widget), over the tiles, so a text box is above
  every stroke whatever the order in the file. A box is rendered once to a picture at the settled zoom (sharp, up to
  4096 device px a side) and cached; the cache is keyed on the text, the box size, the scale and the page colour. A box of
  more than 16 384 characters is drawn cut at a line end with an ellipsis (the editor over it shows all of it), and
  one box is laid out at most 4096 px a side.
  - A paint stops rendering new text-box pictures once the pictures **it rendered** reach `kTextCacheBytes` (32 MB). The
    boxes it did not get to keep their stale picture or show a plain placeholder, and one more paint is scheduled that
    carries on, so every box gets its picture within a few paints and none is starved.
  - After a paint, the cached pictures are trimmed back to 32 MB, least recently painted first; the pictures that paint
    used are exempt from that trim.
  - **A hard ceiling** for all cached pictures together, `InkCanvas::kTextCacheCeilingBytes` (128 MB, four times the
    budget), covers what the exemption would otherwise allow (many large boxes in view). Past it, a box keeps its stale
    picture or shows the placeholder, and no further paint is asked for: nothing more is rendered until the cache
    shrinks again (a box scrolled away or deleted).
- **Big files.** A file over 256 KiB is parsed off the UI thread; the editor opens files up to 64 MiB. The index reads up
  to 16 MiB (see above). The undo history is 100 steps and a document is at most 100 000 pages.
- **Clipboard.** A paste over 16 MiB is refused up front; below that it is parsed on the UI thread (about 12 ms a MiB).
- **Executable cost.** The base program is linked with `ENABLE_EXPORTS` so modules can resolve
  host classes from it. Measured against `main`: start-up unchanged (the dynamic loader 17.28 versus 17.27 million cycles,
  cold start within -2.2 to +2.1 percent), the file +1.3 percent, but the text segment +10.6 percent (the dynamic
  symbol tables are +1.19 MB of that). The size gate was written on file size and passes. If size is judged on the text segment, the
  fix is the `-fvisibility=hidden` plus an export macro rework the spec describes.
- **The bench rows are provisional.** Every Handwriting row in `docs/PERFORMANCE.md` (`noteAppC-bench --phase ink`
  and `--phase inkcarry`) was measured on a loaded machine, with another heavy process running and the pinned cores about 75
  percent idle, so the pool-side rows are pessimistic and noisy. They are provisional until a rerun on an idle machine,
  which is owed; the rows say so themselves.

## Not verified offscreen

Everything above was tested with the offscreen platform, which cannot show a popup, a cursor, a real pen or a real
window width. These are the checks left for a person with the hardware (a FydeTab Duo and its pen, and a Wacom
pen for the buttons):

- **Barrel buttons and the eraser end.** The defaults are the spec's. The test pen's buttons were broken and it has no
  eraser end, so held erase, held select and the glyph studio's barrel click have only been exercised with synthetic
  events.
- **Pen proximity and pressure tail.** What happens when the pen leaves proximity mid-stroke
  (`TabletLeaveProximity`), and the pressure tail at pen-up, have not been seen on the device. A fractional display
  scale (the pen input test ran at scale 2) is also untested; `kFractionalScaleOffset` is 0, 0.
- **A pen tap in an open text box** should place the caret (the window system turns the ignored tablet event into a
  mouse click; a lost pen release makes a tap behave as it did before that fix).
- **Text box editor against the rendered box.** The editor has a 1 px frame, so the text shifts about 1 px when editing
  starts or ends, and its text size differs from the picture by up to about 5 percent at zoom 2 (the editor uses whole
  pixel font sizes). `Escape` with the slash or wikilink popup open should close the popup, not commit the box.
- **Toolbar.** The Shape and Select buttons: the face picks the tool, the arrow opens the menu of kinds (Line, Arrow,
  Rectangle, Ellipse; Lasso, Rectangle). At the real window width the extension arrow shows the overflow, and the
  Page button and the zoom box stay reachable; after using the width box or a combo, the `P` `E` `L` `S` `T` keys work
  again (focus returns to the drawing surface).
- **Page menu.** The Format, Layout and Pattern combos open their own popup over the menu; the menu stays up and the
  change lands. Typing a width in millimetres and pressing `Enter`.
- **Cursors.** The hand cursor over the page while the mouse cannot draw (the pen's hover shows it too), and the closed
  hand while dragging.
- **Dark pages.** On a dark page colour the pen swatches switch to light ink.
- **Zoom.** `Ctrl`+wheel is not anchored at the pointer: look where the view lands.

Known limits (not bugs to hunt): zoom and the pen colour are not remembered between sessions; with the Text tool and no box
open, pasting plain text does nothing; the text boxes use the built-in Light or Dark theme by page lightness, so a customised application theme's colours
do not reach them (font overrides do); a toolbar toggle reaches other open ink tabs when they are next shown.

## Credits

Parts of the stroke engine are ported from Rnote, <https://github.com/flxzt/rnote>, copyright the Rnote contributors,
GPL-3.0-or-later, as is Notolith. Each ported function carries the notice in its source:

- the stroke outline, `compose_lines_variable_width` in `rnote-compose/src/style/smooth/mod.rs`
  (`libs/ext/ink/core/StrokeOutline.cpp`);
- the smoothing parameters, `rnote-engine/src/pens/pensconfig/brushconfig.rs` (`libs/ext/ink/core/InkInput.cpp`), fed to
  Google's ink-stroke-modeler, Apache-2.0, pinned at `f2388813b0b25bc3e33d143d369a8367ab2e30c8`
  (<https://github.com/google/ink-stroke-modeler>);
- the pressure curves, `PressureCurve::apply` in `rnote-compose/src/style/mod.rs` (`InkTypes.cpp`);
- the eraser that cuts strokes, `split_colliding_strokes` in `rnote-engine/src/store/trash_comp.rs` (`InkTools.cpp`);
- moving and resizing a selection, `Stroke::scale` and `Stroke::translate` in `rnote-engine/src/strokes/stroke.rs`
  (`InkTools.cpp`);
- the page patterns, the `gen_*_pattern` functions in `crates/rnote-engine/src/document/background.rs`
  (`PagePatterns.cpp`).

## Glyph studio

The second module, `notolith-hwfont` (package `notolith-hwfont`, feature `handwriting-font`), is a tab for capturing
your handwriting letter by letter. It is the first step towards a font made from your own hand; the training and font
building come later. It links `libnotolith-inkcore.so` from the package `notolith-ink` and never loads the
`notolith-ink` module itself, so the studio works whether or not "Handwritten notes" is switched on.

- Open it with the command "Open glyph studio". It needs an open vault: the samples are saved there.
- The studio is a tab that belongs to no file. It counts as modified while the cell holds strokes; the host saves the
  cell when the tab closes or Notolith quits, so a sample is never lost.
- Write the glyph shown, with guide lines for the ascender, the x-height and the baseline behind it. The list on the
  right shows every glyph and how many samples it has against how many it wants ("a 1/3": one of three written); a click
  on a row targets that glyph, and the cell keeps what you had drawn. Glyphs that still need samples come first.
- **Keys**, all with buttons beside the cell (the pen's tip is the only thing the FydeTab pen has):

  | key | button | does |
  |---|---|---|
  | `Enter` | Save | saves the sample and moves to the next glyph that wants more |
  | `Backspace` | Discard | clears the cell without saving |
  | `Ctrl+Z` | Undo stroke | undoes a stroke |
  | `Ctrl+Shift+Z`, `Ctrl+Y` | Redo | redoes it |
  | `Page Down` | Skip | the next glyph that wants more, without saving |
  | `Escape` | | does nothing |
  | the upper barrel button, clicked | | saves like `Enter`, when the pen has one |

  A barrel press that travels more than 4 px is not a click, and in the studio holding the button never turns a stroke
  into a selection. The keys work as soon as the tab is shown.
- "New font..." asks for a name, switches the studio to it (keeping the cell) and stores it as `hwfont.fontName`. A
  name that could leave `Handwriting/` (empty, containing `/`, `\` or `..`, containing a control character, or starting
  with a dot) is refused and the studio shows why.
- The cell draws with the mouse as well as the pen, whatever `ink.drawWithMouse` says.

**What is saved.** One `.ink` file per capture, so a letter can have several hand-written variants:
`<vault>/Handwriting/<font>/<hex>/<n>.ink`, where `<hex>` is the code point in lower-case hex, four digits (`0061` for
"a"), six when it needs them, and `<n>` is the lowest free number. The files sync with the vault, which is how they reach
the machine that will train on them. Each is a 400 x 400 `fixed` page, white, pattern `none`, in the format above, with
one extra key under `document`:

```json
"glyph": {"codepoint": 97, "baseline": 280, "xHeight": 160, "ascender": 100}
```

(`baseline`, `xHeight` and `ascender` are the y of the three guide lines on that page, in document px). The sample is
written through the vault's no-follow writer, so a `Handwriting/` folder that is a symlink out of the vault is not
written through, and a sample another studio or a sync wrote in the meantime is never replaced: this one takes the next
number.

**The glyph set** is ASCII `!` to `~` (digits, letters, punctuation), seven Latin-1 punctuation marks (U+00A1, U+00A7,
U+00AB, U+00B6, U+00B7, U+00BB, U+00BF) and the Slovene letters c, s and z with a caron, both cases: 107 glyphs.

**Settings.**

| key | default | meaning |
|---|---|---|
| `hwfont.fontName` | "My handwriting" | the font the studio writes into: a choice among the folders already in `Handwriting/`; a new font is a new folder ("New font...") |
| `hwfont.samplesPerLetter` | 3 | samples asked for each letter (ASCII letters and the Slovene ones), 1 to 10 |
| `hwfont.samplesPerPunctuation` | 1 | samples asked for each digit and punctuation mark, 1 to 10 |
