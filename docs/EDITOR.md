# The editor

`libs/editor/MarkdownEditor` is the one editing widget: live preview, source
and reading are three modes of it. This page covers the optional parts of how
it takes input: [HTML edited in place](#html-edited-in-place),
[the `</>` edit button](#the--edit-button-on-rendered-blocks) and its
[callout menu](#the-callout-menu), [embedded notes](#embedded-notes),
[the status bar counts](#status-bar-counts),
[bare brackets](#bare-brackets), [meta notes](#meta-notes--) and
[Vim key bindings](#vim-key-bindings).

## Scripts never run

JavaScript is never run; only HTML and CSS
formatting is followed, even where a page relied on a script. There is no
script engine anywhere in the app: litehtml has none, and `sanitise()`
(libs/render/HtmlRenderer.cpp) strips `<script>` and every `on*=` attribute
before litehtml sees the markup. What a script element says is shown in its
place instead, in `--text-error` and bold: an HTML block draws it as text
(libs/editor/HtmlBlock.cpp layoutHtml), an inline `<script>...</script>` keeps
its tags showing (inlineHTMLpairs). Links open only http, https and mailto,
from notes and PDFs alike, so a `javascript:` URI is never handed on.

## HTML edited in place

HTML renders as in Obsidian, but unlike Obsidian it is edited inline, just
like Markdown; an HTML table is edited like an actual table element (like in a
spreadsheet, clicking on cells, etc.).

`libs/editor/HtmlEdit.cpp`. After every parse, each top-level HTML block is
read with gumbo (litehtml's own parser, so no new dependency). A block whose
every tag is in the owned set below becomes the decorations a Markdown note
gives: `<p>` a paragraph, `<h2>` a heading, `<ul>`/`<ol>` a list with drawn
bullets or numbers, `<blockquote>` a quote, `<hr>` a rule, `<pre>` a code
panel, `<table>` a table. Its tags are syntax: hidden, and shown when the caret
touches them, the way `**` is. A `<table>` is edited by the same code as a
pipe table (`TableCells.cpp`): click a cell, Tab and Shift+Tab, the arrows,
Enter down a row or a new row under the last, Backspace that never eats a tag,
and the row and column commands and menu.

**Owned, edited in place:** `p`, `div` without `style=`, `br`, `h1`-`h6`,
`ul`, `ol` (`start=` read), `li`, `blockquote` (text, or `<p>`s), `hr`, `pre`
(with one `code` inside), `table` with `thead` `tbody` `tfoot` `tr` `th` `td`
(`align=` or a `text-align:` style on the first row's cells read, which
sets the column's alignment), and inline `b` `strong` `i` `em` `u`
`s` `del` `strike` `mark` `sub` `sup` `kbd` `code` `a` (`href` is the link)
`span` (a `style=` holding only `color:`). Other attributes (`class`, `id`)
are kept and ignored. A `style=` on a `th` or `td` is accepted when it holds
only `text-align`, `color`, `background`, `background-color` and
`font-weight` (a value with a `:` or `url(` is not), and each is drawn:
the alignment, `background`/`background-color` as a fill under the cell's text
(a colour name, `#rgb`, `#rrggbb`, `rgb()` or `rgba()`; anything else, such as
a shorthand with more than a colour, is not drawn), `color` as the text
colour, and `font-weight` `bold`, `bolder` or 600 and up as bold. Colours are
as the author wrote them, with no contrast adjustment for the theme.

**Left to litehtml** (drawn as an image, clicked into it shows its source, as
before): a block with any other tag (`img`, `svg`, `iframe`, `video`,
`details`/`summary`, unknown tags), `style=` on anything but a colour span or such a cell, a
comment, a table inside a cell or any block inside a cell, text after a nested
list in an `<li>`, `caption`/`colgroup`, markup gumbo had to repair (misnested
tags, text stranded in a table), and two block elements on one line
(`<p>a</p><p>b</p>`): each block element must start its own line, because a
block of the editor is whole lines.

**Inline `<img>`** (Obsidian draws one wherever it appears): an `<img
src="docs/hit.png" width="400">` inside a paragraph, list item, quote or
callout, or a pipe-table cell, is drawn as the image when the caret
is not in it. It is the same inline embed `![[p.png|400]]` and `![](p.png)`
produce (the parser emits the same `EmbedSpan`, marked `html`, and
`collectTextEmbeds` / `layoutTable` draw it), so resolving, the size cap,
the image cache and the reveal are shared with them:

- `src` resolves as a Markdown image's does (vault-relative, note-relative,
  attachment folder, through the window's link resolver). Only a relative
  path or an `http(s)` URL is an image at all, and a remote URL is the
  resolver's call exactly as for `![](https://...)`: nothing is fetched here.
  `javascript:`, `data:`, `file:` and `//host` sources are never drawn; the
  tag stays as source text. A target that is not a decodable image (a PDF, a
  note) is a missing file, not an embed of it.
- `width` and `height` are pixels, bare or with `px` (a `%` is ignored, so
  the image shows at its own size). One of them keeps the aspect ratio; both
  are taken as given and, with the width capped, scaled down together (a
  height is also capped at 4096 px, ratio kept, as for `|WxH`); the width is capped at the readable column (at 66% of
  it, in a table cell). `alt` is shown on a small card when the file is
  missing or unreadable (the file name when there is no `alt`).
- The tag is source while the caret is in it, or anywhere in the block (the
  cell, for a table), like any embed.
- A tag with no `src`, an unclosed quote or a stray token stays source text.
- A top-level `<img>` on a line of its own is still an HTML block drawn by
  litehtml (above), not this path, while the same tag after `- ` in a list or
  `> ` in a quote takes this one; an `<img>` inside an HTML `<table>` also stays
  with litehtml (the table is an HTML block). Not done: `%` widths, and
  `<img>` as a link target or in the backlink index.
- A Markdown image, `![[p.png|40]]` or `![](p.png)`, in a table cell is drawn
  by the same code now (it showed as source text before); a PDF, note or
  other file embed in a cell is still text.

Tests: `HtmlImgTest.*` in `tests/test_editor.cpp`.

**Animated GIFs** play, as in Obsidian: `![[x.gif]]`, `![](x.gif)` and
`<img src="x.gif">`, alone in a block, inline in text, in a list or quote, or in
a pipe-table cell (`libs/editor/GIFplayer.cpp`, one `QMovie` per file).

- A movie runs only for a GIF with more than one frame that is drawn inside the
  viewport of a shown editor, to the screen. A one-frame GIF, a GIF past the
  cap, one that is off screen, and any render through another paint engine (a
  PDF, a printer, a `QPicture`) show the first frame, as every other image does.
  (A `grab()` of a shown editor is a raster paint of the same widget and shows
  the frame on screen.)
- At most 8 movies play at once; the ninth GIF on screen stays on its first
  frame. The same file embedded twice is one movie.
- A movie ends when its embed scrolls out of the viewport (on the scroll itself),
  when the editor is resized so it no longer shows, when the editor is hidden,
  when the note is replaced or the link resolver changes, and when an edit
  removes the embed (at its next frame). It starts again from the first frame
  when it is shown again.
- A GIF that plays once (no loop extension) stops on its last frame and frees
  its slot; a file that fails mid-way stays on its first frame. Neither is
  restarted by the next paint, until the note or the resolver changes.
- What is known of a file (more than one frame or not) is kept per file and
  `mtime`: one scan, not one per paint, and a replaced file is looked at again.
- Each frame repaints only the embed's own rectangle, and `paintTable` draws
  only the cells inside the rectangle being repainted, so a GIF in a 500-row
  table repaints its own row.
- A note with no animated GIF on screen constructs no `QMovie`, no timer and no
  connection.
- A GIF in an HTML block (top-level `<img>` line, litehtml) is still one frame.
  Frames are not cached beyond what `QMovie` keeps, so a huge GIF costs its
  decode while it plays; the cap bounds that at 8.

Tests: `GIFtest.*` in `tests/test_editor.cpp` (the fixtures are tiny GIFs held
as bytes in the test, since Qt has no GIF writer).

**Borders in the litehtml image** (`libs/render/HtmlRenderer.cpp`): every
CSS 2.1 border style is drawn. A uniform `solid`, `dotted` or `dashed` border is
one stroke round the box; any other case (sides that differ in width, colour
or style, or `double`, `groove`, `ridge`, `inset`, `outset`) cuts the ring
between the outer and inner edge into four mitred wedges and draws each side in
its own style, so per-corner radii (`border-radius: 20px 20px 20px 0`) and
mixed sides compose. `double` is two thirds-bands with a gap, `groove` and
`ridge` two halves (outer dark then light, ridge reversed), `inset` and
`outset` shade the top and left or the bottom and right darker; a near-black
colour brightens the light side instead.

What it does with the bytes - the file is the truth:

- Typing `&`, `<` or `>` into drawn text writes `&amp;`, `&lt;`, `&gt;`
  (typing inside a tag the caret has revealed edits the tag, unescaped).
- An entity (`&amp;`, `&nbsp;`, `&#169;`, every name HTML defines) shows as
  its character; the caret steps over it as one, and Backspace or Delete
  removes all of it. With the caret on it (live preview, no selection) it
  opens into its source, `&amp;`, and edits as text; it closes again when the
  caret leaves.
- Whitespace shows collapsed, HTML's way, and is never rewritten: a run of
  spaces or a newline shows as one space, but stays in the file as written.
  CRLF and a BOM are untouched.
- `<br>` is one character on screen, a line break; the caret steps over it,
  and it is not revealed under the caret.
- A line break pasted into a cell becomes a space, as in a pipe table; in other
  text a `<br>`; in a `<pre>` a newline.
- Enter in a `<p>` or `<li>` splits it (`</li>\n  <li>`, at the element's own
  indentation, closed only if it was closed); inside `<b>` or a link, and in a
  heading, it writes `<br>`.
- A new row or column copies the table's tag case and indentation and uses
  `th` in header rows, `td` elsewhere; cells and rows are always closed. An
  HTML table is never aligned (`TableFormat` is for pipes only).
- Backspace at the very start of a `<p>` or `<li>` deletes the `>` of its
  tag, which the caret has revealed by touching it - as Backspace after `**`
  deletes a `*`. Only a table cell refuses (its edge is a tag).
- Vim mode's `h`, `l` and `x` (and an operator over them, `dl`) step over an
  entity as one character, as the arrows, Backspace and Delete do; `l` stops
  on a last-on-the-line entity as on any last character.
- The cell being edited shows its inline tags as written, as a pipe table's
  does; its entities stay one character. A `<pre>` under the caret shows its
  source, as a fence does.

**Under the caret an entity opens into its source** (HTML's and Markdown's alike),
as the markers of other inline syntax do: with a bare caret anywhere from its
`&` to its `;` (both edges count) it shows as written, `&amp;`, and the caret
walks through its characters; one step off it closes again. A selection keeps
every entity closed (selecting across `&copy;` takes it whole), Source mode is
always as written, and Vim's normal and visual modes keep stepping over one as
a single character (insert mode opens it like the other modes). So Backspace
and Delete next to an entity take one character of its source while it is
open; to remove one whole, select it. Inside a table cell the entity stays
one character, as before. Implementation: `revealedEntities()` in
`HtmlEdit.cpp` adds the touched substitution to the block's revealed set, and
the block's layout draws the list without it; `moveCursor` relays out the
caret's block when that set changes, which also makes a caret moving by arrow
keys inside one paragraph reveal what it reaches without a click.

Cost: the structure pass is in the parse (off the UI thread), proportional to
the block, cached per thread by the block's text, so a keystroke anywhere else
parses no HTML; a note without HTML pays one test per block. Measured: a
keystroke in a cell of a 50-row HTML table 1.33 ms (budget 8 ms), the parse
and structure pass of that note 0.24 ms cold. Off: the `html-blocks`
feature (`Features::HTMLblocks`) turns this and litehtml off together (source shows);
`NOTEAPPC_WITH_HTML=OFF` builds without gumbo, and HTML shows as source.

Headings: an `<h1>`-`<h6>` in a top-level HTML block is listed in the outline
and indexed like a Markdown heading, so `[[Note#Heading]]` finds it and a
section search sees it. They come from the Parser (`htmlHeadings` in
`libs/markdown/Parser.cpp`, a scan of the block's text with comments skipped),
not from the editor's gumbo pass, because the index must agree with the editor
and has neither gumbo nor the editor's HTML option: they are headings of the
note however the block is drawn (its own layout, litehtml, or as source). A
heading inside a quote or a list item is not a top-level block and is not
listed. The heading text is the content with its tags removed and its entities
decoded; the heading's range is the element, its text range the content.
Publish draws the element once, with its HTML block (`tests/test_publish.cpp`).
`![[Note#Heading]]` (an embedded section) and a canvas card's `#Heading`
(the section picker) find them too: both go through `headingSection` in
`libs/markdown/Parser.cpp`, one line scan shared by the two - an ATX heading or
an `<h1>`-`<h6>` element at the start of a line, whose text (tags removed,
entities decoded, case ignored) equals the one asked for, running to the next
heading of either kind at its level or above, or the end of the note. Headings
in a fence, a comment or the frontmatter do not count (a YAML `# comment` used
to).

Entities in Markdown text: a CommonMark reference (`&amp;`, `&copy;`, `&#169;`,
`&#xA9;`, any name md4qt knows) outside code, maths, HTML, links, wikilinks and
frontmatter, and not escaped with a backslash, shows as its character and is
one step for the caret, Backspace, Delete and Vim's `h` `l` `x`, exactly as in
an HTML block (`Parser.cpp`'s `scanEntities`, drawn through DisplayMap's
substitutions). The source is never rewritten, Source mode shows it as
written, and typing next to one writes plain text, not an entity. A reference
with no `;`, an unknown name, a number
that is no character and a control character (`&#10;`) show as written. Entities
inside a Markdown link's text are left as written.

Backslash escapes: a backslash before ASCII punctuation (`\(`, `\*`, `\\`, `\$`,
`\#`...) is CommonMark's escape and Obsidian does not show it, so neither does
live preview: `\(A=26\)` reads "(A=26)", `\*not italic\*` reads "*not italic*",
`\\` is one backslash. They are substitutions like entities (`scanEscapes` in
`Parser.cpp`, opened by the caret touching one through the same
`revealedEntities()`), with the same exclusions: code, maths, HTML, links,
wikilinks, `%%` comments and the frontmatter keep their backslashes, as does
Source mode. A backslash before anything else (`C:\Users`, a line-end `\`) is
text. Obsidian renders only `$...$` and `$$...$$` as maths, so `\(...\)` and
`\[...\]` are not.

Tests: `tests/test_html_edit.cpp` (`ctest -R html_edit`); escapes in
`tests/test_math.cpp` (`ctest -R math`).

## Display maths

A `$$ ... $$` that owns its lines is a block of its own even inside a paragraph:
md4qt does not end a paragraph where a formula starts, so `Parser.cpp`'s
`splitParagraphsAtDisplayMath` cuts the paragraph into the text before, the
formula and the text after (`\(A=26\) (a-z):` on the line above `$$` used to
keep the whole thing as source in a code-looking box). A formula that shares a
line with other text stays in its paragraph.

MicroTeX draws it; a formula it refuses is retried as MathJax would read it
(`align*` and the other starred environments unstarred, an `&` outside any
environment inside an `array`). One that still fails shows its source with a
dim "Math: <reason>" line under it, never an empty gap. While the caret is in
the formula and what is typed does not parse yet (`\begin{array}{c} a \end{array} &`),
the last drawing stays up and no reason shows, so nothing below moves. Display
maths md4qt did not parse (a blank line inside) keeps its backslashes, as do
autolinks.

**With the caret in a formula** (or a diagram fence: mermaid, tikz, plantuml,
dot) the source shows, both `$$` lines included, and what it draws stays
visible under it, following what is typed (`PreviewBelow.cpp`). A formula is
drawn on the spot, one kept at a time. A diagram is made by its external tool,
so it is asked for only once typing has paused for half a second, and the
last drawing stays up meanwhile; the request is the fence renderer's own, so
the drawing is already cached when the caret leaves. A drawing the next one
replaces is dropped from the fence cache, which is also capped at 64 entries. Source mode shows
neither. The diagram sits under the listing, above the closing fence line.
Tests: `tests/test_math.cpp`.

## Lists

Requirements: pressing Return after a counter starts the next one, numbers
continue logically, different markers (-, *, =) render differently, and there
is a hierarchical number ("first number.second number", so 1.2), toggleable,
ending in a "." or ":" or anything else.

**Why Enter did not continue a list.** It did, but only for a bare list: the
list-line pattern (`EditingCommands.cpp`, `parseListLine`) did not know a quote
prefix, so `> - x` and `> 1. x` in a quote or callout fell through to a plain
newline. A line now reads `quote + indent + bullet + spacing + [checkbox]`.

**Keys** (all in `EditingCommands.cpp`, one undo step each, byte-exact apart
from the numbers they renumber):

| key | on a list item |
|---|---|
| Enter | a new item with the same marker and indent (`-`, `*`, `+`, `N.`, `N)`, a task becomes `- [ ] `, inside `> ` too). Ordered counts on and renumbers the items after it. Mid-item it splits the item. An empty item leaves in steps: out of a quote level first (`> - ` becomes `- `), then out one indent level, then the marker goes. |
| Ctrl+Enter | a task's box goes to the state's next one (`[ ]` to `[x]` and back, `[/]` to `[x]`, `[-]` to `[ ]`; see Checkbox states); a line that is not a task becomes one |
| Shift+Enter | a line break inside the item, the text lined up under the first line's; no new bullet |
| Tab | indents the item from anywhere in it (a tab; four spaces in a quote, where the parser counts columns). An ordered item restarts at 1 in the sub-list, the items below close up. Refused for the first item and for an item already a level below the one above, as CommonMark and Obsidian refuse. |
| Shift+Tab | outdents; the item continues the parent's numbering |
| Backspace | at the start of an item's text takes the marker off and leaves the text; the items after it close up |

Tab and Backspace are left alone in a fenced code block and with a selection;
table cells keep their own Tab (`TableCells.cpp`). Enter in a code fence is a
plain newline.

Renumbering covers the item's own list run only - its items, the wrapped lines
and blank lines between them, in the same quote - and only from the edited line
down, and it stops at the first later sibling that already carries the right number (as Obsidian does): items above it and a different list (another bullet or delimiter, as
CommonMark counts) are not touched. A 5 000-item list takes about 2 ms.

**Display (live preview only; the file stays CommonMark).** The marker shows
its real source under the caret, as every marker does. Drawn numbers use
tabular figures, so a column of them lines up.

| setting | default | what |
|---|---|---|
| `editor.listNumberStyle` | `plain` | `plain` draws each list counting on its own (`1.` `2.`); `hierarchical` composes a nested item's number from the items it sits in (`1.` then `1.1.` `1.2.` then `1.2.1.`). Replaces `editor.outlineNumbering`. |
| `editor.listNumberSuffix` | `.` | what follows a drawn number: `.` `:` `)` or any short text. Empty draws the delimiter the note was written with. |
| `editor.bulletGlyphs` | `-=dash,*=dot,+=diamond` | the glyph per marker, from `dot` `ring` `square` `diamond` `dash` `arrow` `star` `check` `turn` `typed` (the character itself). Settings > Editor shows a drop-down per marker with a preview, and Reset restores the default. |
| `editor.bulletGlyphsNested` | on | a nested bullet (depth 1 and deeper) is drawn as a turn arrow, the shape of U+21B3, instead of by marker; depth 0 keeps its marker's glyph. A marker set to `typed` stays as typed. The arrow is also `turn` in `editor.bulletGlyphs`. |

`=` is not a CommonMark list marker (`- [ ]`, `* ` and `+ ` are), so it cannot
be mapped; the three real ones can. The glyphs are painted in
`BulletGlyphs.cpp` with the muted text colour; the dash is a rounded bar wider
than a dot, the dot a filled circle, the diamond a small rhombus.

A list continues or ends only outside a code fence. The fence check reads the
parse (up to 20 ms old) and, for the fence lines in the unbroken run of at most
100 lines above the caret, any that the parse has not seen yet, so a fence typed
within that delay counts at once; a fence above a blank line the caret has
already crossed is seen when the parse lands.

`tests/test_lists.cpp` covers every key (asserting the whole note, the caret and
undo), the glyph map, the pixels the editor paints per marker, the number labels
and the timing.

## The `</>` edit button on rendered blocks

When hovering over rendered elements the code icon appears, to edit the code.
Behaviour is Obsidian's "Edit this block"; the code
is `libs/editor/HoverEdit.cpp`.

Pointer over a rendered callout (`> [!x]` and `:::`), block formula, HTML
block, plugin fence (mermaid and the like), `query` / `base` block, note, file,
PDF or image embed, or a table: the block's panel gets a 1 px ring
(`--background-modifier-border`, radius `--radius-s`) and a `</>` button at its
top-right, 4 px in (a foldable callout's button steps left of its fold arrow).
Clicking the button selects the block's whole source with the caret at its
start, which shows the source; moving the caret hides it again. A click
anywhere else in the block behaves as it always did (caret at the click).
Plain code blocks (they have their own label and Copy), frontmatter and
notebook output never get one. A table's cells still edit in place; the button
is the way to its raw pipe text.

Also: right-click a callout for "Edit block source", and the command
`noteAppC:edit-block-source` (unbound; bind it under Settings > Hotkeys) does
the same for the caret's block.

Setting `editor.hoverEditButton` (Settings > Editor, on by default). Off, the
pointer is not tracked for these kinds at all. Hover is a mouse-move path, never
a keystroke path: one `lower_bound` over the sorted blocks, a repaint of the two
panels that changed (not the viewport), a painted button, nothing laid out.

Tests: `HoverEditTest.*` in `tests/test_editor.cpp`.

## PDF embeds: slides navigation and zoom

### Slides layout: forgiving Previous / Next

The box to click "Next" is larger around it, so a
slightly-off pointer does not open the embed by accident (like a phone
keyboard's keys). Code: `PdfEmbedBlock.cpp` (`pdfNavHit`).

- The row under the slide (nav strip plus the 6 px gap above it, the full width
  of the slide) is the buttons: its left half is Previous, its right half is
  Next, wherever the label is drawn. A half whose button is hidden (first or
  last slide) swallows the click: no step, no caret.
- A click within 10 px (`kPdfDeadBand`) of the slide's bottom edge is swallowed.
- For 1000 ms (`kPdfStickyMs`) after a Previous/Next click, that button also owns
  the bottom 25% (`kPdfStickyShare`) of the slide on its side, so quick repeats
  keep stepping when the pointer drifts up. Afterwards a click there is an
  ordinary click on the slide (the caret enters the block, showing its source).
- The pointer is a hand where a click steps and an arrow in the swallowed areas.
- A finger goes through the same path; `touchable` widens the dead band by the
  touch slop.

Tests: `PdfEmbedTest.*` in `tests/test_editor.cpp` (the clock is injected with
`setPdfNavClock`, no sleeping).

### Zooming a page

PDFs can be zoomed in embedded mode. Code:
`PdfEmbedBlock.cpp` (`zoomPdfAt`, `wheelPdfCell`, `pressPdfCell`,
`doubleClickPdfCell`, `requestPdfZoomRenders`).

The cell is a fixed window: zooming never changes the embed's height, so the
note does not reflow under the pointer. Zoom runs from 1x to 4x (the full PDF
viewer's own ceiling), and every page of one embed (a column, a grid, a deck)
shares one zoom and one pan - the same corner of every page.

| input | does |
|---|---|
| Ctrl + wheel over a page | zooms, 1.1x a notch, the content under the pointer stays put |
| pinch on a touchpad | the same (Wayland delivers it; X11 has no native zoom gesture) |
| double click | 1x to 2x at the pointer, and back to 1x |
| Ctrl+0 | back to 1x for every PDF embed in the note |
| drag, while zoomed | pans; never moves the caret or shows the source |
| Shift + wheel, or a horizontal wheel, while zoomed | pans (up and down, sideways), stopping at the page edge |
| plain wheel | always scrolls the note, zoomed or not |

While zoomed the pointer is an open hand over the page and a "200%" badge sits
in its corner; a click that does not move does nothing (the `</>` button is
the way to the source). The slide controls above come first: a press they
claim never starts a pan. At 1x everything is as before: a click on a page
places the caret and shows the source, so the first click of a double click does
that for a moment and the second puts the caret and the scroll back where
they were.

The zoom and pan are kept for the life of the editor, by the embed's place in
the note (an edit above it moves them along, as it does the slide), and never
written to the note. They also keep from slide to slide. A `#zoom=` argument
would be the way to write one down; today `#zoom=150` is ignored like any
unknown key.

Rendering: a zoom repaints the page already held, stretched, at once; 150 ms
after the last step the page is rendered at the zoomed width (in 5% steps, at
most 2600 px) and swapped in. A page scrolled into view later at that zoom is
fetched the same way. The zoomed rasters are kept to 128 MB (oldest out first;
a page then shows its normal raster stretched until the next fetch), and only
the pages on screen are fetched, together within that budget. The ordinary
page rasters are not bounded, as before.

Tests: `PdfEmbedTest.*Zoom*`, `Wheel*`, `Drag*`, `DoubleClick*` in
`tests/test_editor.cpp`.

## Checkbox states

Tasks can show more than a tick: a line, a red X, and other useful states.

Obsidian reads any one character in a task's box as a task and leaves the
look to a theme. With the **Checkbox states** core plugin on (default; Settings
> Core plugins, id `task-states`) so does this editor, and draws them itself:

| box | name | type | looks | text | click goes to |
|---|---|---|---|---|---|
| `[ ]` | todo | todo | the box | | `[x]` |
| `[x]` `[X]` | done | done | filled, tick | struck, muted | `[ ]` |
| `[/]` | in-progress | in progress | left half filled | | `[x]` |
| `[-]` | cancelled | cancelled | a line across | struck, muted | `[ ]` |
| `[~]` | dropped | cancelled | a red X | struck, muted | `[ ]` |
| `[>]` | forwarded | todo | arrow right | | `[x]` |
| `[<]` | scheduled | todo | arrow left | | `[x]` |
| `[?]` | question | todo | a blue `?` | | `[x]` |
| `[!]` | important | todo | an orange `!` | | `[x]` |
| `[*]` | star | todo | a yellow star | | `[x]` |
| `[I]` | idea | non-task | a yellow `I` | | `[x]` |
| `[p]` | pro | non-task | a green `p` | | `[x]` |
| `[c]` | con | non-task | a red `c` | | `[x]` |

`[ ]` and `[x]` are Obsidian's own and fixed; the rest are `tasks.ini`'s to
change (docs/STYLING.md). Any other single character, `[q]`, is still a task:
a todo drawn as its letter in a box, which a click turns to `[x]` (Obsidian
would call it done; the Tasks plugin's own rule is this one). `[[` and `](` are
never a box, so `- [[Note]]` and `- [x](url)` stay text, and neither is a character outside the
Basic Multilingual Plane (an emoji): a state is one UTF-16 unit, so `- [<emoji>] text` is a plain
bullet.

Ways to set one: **click** the box (the table's last column; the caret does not
move), **Ctrl+Enter** with the caret on the item (same cycle), **right-click**
the item > Checkbox state (every state, drawn, the item's own checked; on a
plain bullet it gives it a box), or the command "Set checkbox state..." (bind it
in Hotkeys, find it in the palette), which opens that menu at the caret. Enter
after any state continues with `- [ ] `. With the plugin off the command is
Ctrl+Enter's toggle.

The type decides what a search finds (docs/SEARCH.md: `task-todo:` is todo and
in progress, `task-done:` is done and cancelled, plus `task-progress:` and
`task-cancelled:`) and what publishing styles. The plugin also sets
`ParseOptions::taskStates`, so with it off the parse, the editor, the menu and
published pages are what they were: only `[ ]`, `[x]` and `[X]` are tasks, `[-]`
is text.

Published pages and PDFs follow the note: a custom box is a bordered `.task-box`
(`data-task="-"`) holding a character in the state's colour, the text in a
`.task-text` span struck or muted; a page with only `[ ]` and `[x]` is byte for
byte what it was. The state is one character, so an emoji cannot be a box.

Tests: `CheckboxTest.*`, `ListEnter.AfterACustomStateTheNextItemIsOpen`
(`tests/test_editor.cpp`, `tests/test_lists.cpp`), `TaskStatesTest.*`,
`ListTest.*Box*` (`tests/test_markdown.cpp`).

## The context menu

Right-click anywhere in a note. Obsidian's order, minus its Paragraph and
Insert submenus (there is no list, heading or quote helper to reuse; add them
when wanted):

| group | items | when |
|---|---|---|
| spelling | the suggestions, "Add "w" to dictionary" | on a misspelt word (as before) |
| | "Remove "w" from dictionary" | on a word in `~/.config/noteAppC/dictionary.txt` |
| edit | Cut, Copy, Paste | Cut and Paste not in reading view; Cut and Copy greyed with nothing selected |
| format | Format > Bold, Italic, Strikethrough, Highlight, Code, Math, Comment; Add link | not in reading view; with nothing selected a format applies to the word under the pointer |
| | Checkbox state > one item per state, the item's own checked | on a list item, with Checkbox states on; on a plain bullet it gives it a box |
| window | Look up "t", Search for "t", Search the web for "t" | each off by its switch (below) |
| block | the table and callout items | on a table cell or a callout |

`t` is the selection when it is one line of at most 60 characters, else the
word under the pointer; a paragraph is never a lookup target. A right click
outside the selection moves the caret there; inside it keeps the selection.
The Format items are also commands (`editor:toggle-strikethrough`,
`-code`, `-inline-math`, `-comments`, no default keys), so they reach the
palette and Hotkeys.

`MarkdownEditor::contextMenuAt(offset)` builds the menu without showing it,
for the tests; the window-level items arrive through `setContextActions()`
and the `searchRequested` / `lookupRequested` signals, so the editor links
nothing.

### Look up, and what leaves the machine

"Look up" asks a DICT server (RFC 2229, `libs/ui/DictClient`) for the word on
every database it has - on dict.org that includes WordNet (definitions and
`[syn: ...]`) and a thesaurus. Servers come from the "Dictionary server"
setting (`lookup.server`, `host` or `host:port`), else `~/.dictrc`, then
`/etc/dict/dict.conf`, tried in order; a refused one costs under a
millisecond, a host that never answers the connection 1.5 s, one that accepts
and then says nothing (or never finishes) 4 s. A reply over 1 MiB is dropped
as not being a dictionary.

Nothing is sent on a right click. Only when "Look up" is chosen, a TCP
connection to port 2628 of the first server that answers carries exactly
`DEFINE * "word"` and `QUIT` (the word quoted, control characters turned to
spaces), in plain text. With `/etc/dict/dict.conf` as shipped that is
localhost, and when `dictd` is not running, dict.org over the internet. To stay
offline install `dictd` with a database (`dict-gcide`, `dict-wn`) and enable
it. The popover (a second `PagePreview`, `lookupPopover`) says "Looking up..."
at once, fills when the answer comes, and goes with Escape or a click outside.
It opens beside where the menu did: the pointer for a right click, the caret
for the menu key or Shift+F10 (not on Windows, where Qt sends that itself).

Switches: Settings > Core plugins > "Look up words" (`plugins.wordLookup`, on;
off, no item, no popover and no client are built), "Search" for "Search
for", and Settings > Editor > "Web search" (`lookup.webSearchURL`, `%1` is
the URL-encoded text; empty removes the item). Tests: `ContextMenuTest.*` in
`tests/test_editor.cpp`, `DictClientTest.*` (a fake server on loopback; no
test reaches a real one) in `tests/test_lookup.cpp`, `LookUp*` in
`tests/test_window.cpp`.

## The callout menu

Right-click a rendered callout (`> [!x]` or `:::`, any piece of a long one):

| item | does |
|---|---|
| Edit block source | as the `</>` button: selects the whole callout, which shows its source |
| Copy | the callout's source on the clipboard, without its trailing newline |
| Change type | a submenu of Obsidian's 13 types and every section of `callouts.ini`, the current one ticked; rewrites the `[!type]` word, or the type word of a `:::` fence, and keeps the title, fold mark and attributes |
| Remove callout | takes the callout off and keeps what is in it: one level of `>` off every line, or the two `:::` fences; an explicitly written title (`> [!note] Title`, `title=` on a fence) stays as the first paragraph; a nested callout inside becomes the outer one's level |

Each is one undo step. `MarkdownEditor::calloutSourceAt`, `changeCalloutType`
and `removeCallout` (HoverEdit.cpp) do the work for a source offset, so a
plugin or a test needs no menu. Nothing runs until the menu opens.

Click a top-level `> [!type]` callout's title (not a `:::` container or a
nested callout) and it becomes a line edit exactly where it is drawn, over the
panel, with the `[!type]` marker still hidden. Return or leaving the field
writes it back as one undo step, Escape throws it away; an implicit title
(`> [!note]`, drawn "Note") opens empty and gains a space and the typed text.
The rest of the header row, and the fold arrow, still fold a foldable callout;
`</>` or the menu's "Edit block source" reaches the raw line. The caret lands
where the title was clicked. Reading mode never opens the field (nor fills a
short table row: `fillShortRow` is a no-op there). An edit from another tab on
the same note (`applyExternalEdit`) writes an open title back to its callout,
which moves with the text; a whole-text replacement (the other tab's undo)
drops it, as the callouts may be gone.
`CalloutTitle.cpp`; tests `CalloutTitleTest.*`.

### Folding headings and lists

A heading folds its section, as in Obsidian: every block after it up to (not
including) the next heading of the same or a higher level, or the end of the
note. A list item with children folds them; one without cannot (a continuation
line is not a child), and a heading inside a callout or a quote does not fold.
Both are on by default. Settings > Editor > Fold heading / Fold indent
(`editor.foldHeading`, `editor.foldIndent`, read from Obsidian's `app.json`
keys of the same names) turn each off through `MarkdownEditor::setFolding(heading,
indent)`: that kind then has no arrow, hides nothing, is skipped by the fold
commands and is not remembered, and its remembered folds return when it is
switched on again.

An arrow in the margin level with the heading's, or the item's, first line
shows under the pointer and stays while it is folded; a folded line ends in a
small "..." box. Click the arrow or the box to toggle; a click on the line's
own text still puts the caret there. In source mode the arrow sits left of the
line numbers. All three modes fold, and share one fold state (`setMode()`
keeps it).

A folded heading's blocks keep their layouts and heights but take no room
(`BlockView::foldHidden`, set in `updateLayout()`'s one pass with a single
`foldLevel` integer, so a keystroke pays one compare per block); they are
skipped by painting and by every hit test (`offsetAt`, `blockAtPoint`,
`handleBlockClick`, table, footnote, callout-title and code-label lookups, the
edit button), so opening a fold costs one pass and no relayout. A folded H2
inside a folded H1 stays folded when the H1 opens. A folded list item hides its
lines after the first, like a folded nested callout does: a hidden range in the
block's `DisplayMap`, applied in source mode and to the caret's block too,
and its children's bullets, checkboxes and guides are not drawn.

Editing rules:

- The caret arriving in hidden text opens the folds that hide it (a key press
  into it, Enter at the end of a folded heading, a jump from the outline, a
  link or a search: `setCursorOffset()`, `scrollToOffset()`, `revealOffset()`).
  In `updateLayout()` that rule runs only once the user has touched the note
  and not in reading view, so a restored caret and restored folds do not undo
  each other, so a restore sets the caret first and the folds after
  (`MainWindow::openNote()`; `WindowTest.ARememberedHeadingFoldAroundTheRememberedCaretStaysFolded`).
  A selection runs over a fold without opening it, and copy, cut and delete act
  on the hidden source too.
- Folding a section the caret is in moves the caret to the heading's, or the
  item's, line first.
- Folds follow their line: the offset-keyed state is remapped by every edit,
  and undo, redo and a whole-text external edit carry it across by line text
  (`takeFolds()` / `applyFoldState()`).

Commands, under Obsidian's ids and with no default key (palette, slash popup
and Settings > Hotkeys list them like the rest, and a vault's `hotkeys.json`
binds them). They fold headings and list items with children, work in reading
view too, and move the caret to the line they fold it under:

| id | does |
|---|---|
| `editor:toggle-fold` | toggles the innermost fold around the caret: the item or heading whose line it is on, else the one above it |
| `editor:fold-more` | folds the innermost fold around the caret that is open, so repeated runs climb out one level at a time |
| `editor:fold-less` | opens the innermost folded fold around the caret |
| `editor:fold-all` | folds every heading and every list item with children, inner ones included; the caret goes to the outermost fold it was in |
| `editor:unfold-all` | opens them all, and forgets them: an open fold has no entry, so nothing is left to remember |
| `editor:toggle-fold-properties` | folds or opens the properties table |

A callout's, container's or code block's own fold is not touched by fold-all
and unfold-all. The commands are `MarkdownEditor::foldCommand()` in
`BlockRenderers.cpp`; tests `FoldTest.Fold*Command*`,
`WindowTest.FoldCommandsAreInThePaletteAndRebindable`. Fold-all and unfold-all
on a megabyte note take about 1.5 ms each (one pass over the headings and the
list items, then the layout pass; `FoldTest.FoldAllAndUnfoldAllOnAMegabyte...`,
`docs/PERFORMANCE.md`).

Folded headings and list parents are remembered with the rest (below), only the
folded ones: an open one has no entry, so folding everything on a big note costs
one stored line per heading and parent (about 80 kB for a megabyte of notes).
Clicks, the caret rules and the remembered state are covered by `FoldTest.*`
(`AHeadingFoldsToTheNextHeading...` and the tests after it).

### Folds are remembered per note

What was folded or opened by hand (a callout, a nested callout, a `#| fold`
code block, the properties table, a heading, a list item) comes back when the note does, after a
switch in the same pane and after a restart, as Obsidian keeps a file's folds.
`MarkdownEditor::foldState()` gives one entry per foldable block - the text of
the line it starts on, how many earlier foldable lines read the same, and
which way it was set - so an edit above a fold, or a different offset in
another tab, does not lose it; `setFoldState()` puts it back after
`setNoteText()` and drops what no longer matches. A note too big to parse at
once takes it when the parse lands. `MainWindow` keeps it with the note's
caret and scroll in `Settings` (`state.notePositions`, `vaults.ini`, last 50
notes, percent-encoded so a line's commas and tabs cannot split an entry):
written on leaving the note and on closing the window, never per keystroke,
moved with a rename or move and forgotten on delete or merge. Tests
`FoldTest.FoldState*`, `WindowTest.FoldsAreRemembered*`.

## Embedded notes

`![[Note]]`, `![[Note#Heading]]` and `![[Note#^id]]` draw the note, section or
block as a reading-view editor of its own (`FileEmbedBlock.cpp`): headings,
lists, callouts, code and maths as the editor draws them, read-only, into an
image a turn of the event loop after the layout (the plain text shows first).
Only an embed that is laid out - on screen, or a screen either side - is
drawn, and the image is cached by file, mtime, section, width and theme.

A note embedded inside an embedded note is drawn the same way, now, on the
same pass: up to five levels deep (Obsidian's own limit). A note already being
drawn around it (it embeds itself, or A embeds B embeds A) and anything past
the fifth level stays the plain text panel, which is what ends a loop.

An embed in the middle of a paragraph draws the same content as a block of
the paragraph, between the text before and after it, with no title bar; a file
card stands in until the drawing is ready, and when the heading or block is not
there.

Titles: a whole-note embed carries the note's name over it, a heading embed
"Note > Heading", and a `#^id` block embed none (Obsidian shows a block embed
bare). Of links, `[[#^id]]` shows "^id" (only a leading `#` is hidden, as
in Obsidian's live preview) and `[[Note#^id]]` shows as written; a heading
link shows "Note > Heading".

## CSV and TSV embeds

`![[data.csv]]` and `![[data.tsv]]` draw the file as a table, laid out and
painted by the same code as a base's table (`layoutDataTable` in
`BaseBlock.cpp`: header row first, the theme's table colours, columns sized to
their content, a wide table scrolling sideways) - `CSVembed.cpp`. The first
line is the header. The parser (`CSVparser.cpp`) is RFC 4180: quoted fields,
`""` for a quote, delimiters and line breaks inside quotes, LF, CRLF or CR
between records, a UTF-8 byte order mark ignored; a short row is padded with
empty cells and a blank line is not a row. A `.tsv` splits on tabs; a `.csv`
takes the most frequent of `,`, `;` and tab on its first line, so a European
Excel export (`;`) opens right.

The file is read and parsed on a worker thread, never in layout: the block
says "Loading..." until the table lands. What it shows is bounded so that no
file can stall the editor or fill its memory: the header and up to 500 rows,
but never more than 1500 cells (a 64-column file shows 23 rows), with "and N
more rows" and "N more columns not shown" in a footer that spans the table;
at most 64 columns, the rest a final "+N columns" header cell; cells cut at
1000 characters with an ellipsis (both applied in the parser, before anything
is stored: a 1 MB line of commas keeps under 256 KB, uncapped 67 MB, as
`CSVparserTest` measures with glibc `mallinfo2`); and the first 32 MB of the
file. The 1500-cell budget is what a full relayout (every width change
repeats one) fits in 10 ms: measured best-of-7, 3.5 ms for 2 columns, about 6
ms for 8 and 64, on an idle-priority core of the dev machine
(`CSVembedTest.ARelayoutOfTheWorstCappedTableTakesUnderTenMilliseconds`
prints them); the upgrade is laying out only the rows near the viewport.

Bytes are decoded by byte order mark (UTF-8, UTF-16, UTF-32), else as UTF-8,
and when the bytes are not UTF-8 (invalid, and holding no well-formed multibyte
sequence: a UTF-8 file with one stray bad byte stays UTF-8) as Windows-1250 for
a Central European locale (Excel's "CSV" in Slovenia) or Windows-1252
elsewhere.

The result is cached by path. The editor watches the cached files and their
folders (a `QFileSystemWatcher` made with the first CSV embed): a file
rewritten by another program, in place or by rename, is read again after
200 ms and only its blocks are relaid out, the old table staying up until the
new one lands. A file that is not there falls back to the plain link, as any
embed, and comes back when it does; one that cannot be read says so in the
error colour and is read again at the next change event or layout, never
cached as the answer; an empty one says "Empty file". A table scrolled
sideways keeps its place through a reparse and a reread.

Core plugin `csv-embeds` (Settings > Core plugins, "CSV embeds") turns it off:
the file is then the plain source listing it was before. Tests
`CSVparserTest.*`, `CSVembedTest.*`, `FeaturesTest.CSVembedsToggle`; the demo
vault's `Embeds/CSV Embeds.md` shows one of each kind.

## Advanced code blocks (Code Styler)

Core plugin `advanced-code-blocks` (Settings > Core plugins, "Advanced code
blocks", on by default) reads Obsidian's Code Styler syntax; the user-facing
description is in STYLING.md ("Code Styler syntax", "Inline code in a
language", "File references"). One switch covers all four parts and it is a
parse-level one, `ParseOptions::codeStyler` (like `taskStates`): off, the
parser never looks at a fence line's parameters, an inline code's `{lang}`
prefix or a `reference` fence, so notes parse, lay out and paint exactly as
before and the paint paths find nothing to do. `MainWindow::parseOptions()`
fills it from `featureOn("advanced-code-blocks")`; PDF export and publish carry
the same flag (`PdfOptions::codeStyler`, `Options::parse`). A note with
`code-styler-ignore: true` in its properties parses as with the plugin off.

- **Parameters** (`Parser.cpp`, `parseCodeParameters` and `applyFenceParameters`)
  fill the same `CodeOptions` a `#|` line fills; a `#|` line wins when both
  give a key. The tokens are also `CodeFenceSpan::attributes`, so the tab-group
  grouping keeps working, and `title:"a tab b"` no longer makes a stray `tab`.
- **Named highlights** are `CodeOptions::highlights`, kind name to line
  numbers, painted by `paintCodeRows` after the default highlight in the
  colour `codeHighlightColour()` finds (a theme token or a hex, alpha 0.16 dark
  and 0.24 light unless the hex has its own). The names and colours are the
  `editor.codeHighlights` setting.
- **Inline code** is `StyleSpan::code`, the part after the `{lang}` prefix; the
  prefix is in `Decorations::syntax`, so it hides outside the caret's element
  like any marker. `inlineCodeFormats` colours it through the fence's own
  highlighter.
- **Reference blocks** are `ReferenceSpan` in `Decorations::references`
  (`Parser.cpp`, `parseReference`) and `ReferenceBlock.cpp`. `layoutReference`
  runs in `layoutBlock` after `layoutPluginCode` and makes a text panel
  (`layoutTextPanel`), marked `isPluginBlock` so no listing chrome is drawn;
  A file is shown only if its canonical path is under the canonical vault
  root (`setVaultRootProvider`; the window and the PDF export pass it), else
  the block says "Outside the vault" and nothing is read; the window checks
  again before it opens a header click's target.
  `BlockView::referenceTarget` is what a header click emits as
  `referenceActivated`, which the window opens. It follows CSVembed.cpp's
  pattern: one cache entry per resolved path or URL, a read on a worker
  (`QtConcurrent` through `m_lifeline->post`) or by `QNetworkAccessManager`, a
  generation counter dropping a superseded answer, a `QFileSystemWatcher` and a
  200 ms debounce for the files. A URL's copies are also kept in a
  process-wide table, fresh for five minutes. Nothing is read in layout, and
  `setReferenceReadHookForTests` is how the tests see which thread the read ran
  on. PDF export waits (at most 10 s) for `referencesPending()` to clear before
  it lays out for the page. `setRemoteReferences(false)` (the default, from the
  `editor.remoteReferences` setting) keeps every URL from being requested.
  Limit: a target missing at layout time is not watched for; it shows when
  the block is next laid out.

Tests: `CodeStylerTest.*` (parser, tests/test_markdown.cpp),
`CodeStylerEditorTest.*` and `ReferenceBlockTest.*` (tests/test_editor.cpp, the
remote ones against a local `QTcpServer`), `SettingsFeatureTest.*`; the demo
vault's `Code/Code Styler.md` shows every part.

## `[[` link completion

Typing `[[` (or `![[`) opens a popup of note names; a name is matched anywhere
in the text, not only at its start. Four more things complete, as in Obsidian:

- **Headings.** After `#` the popup lists that note's headings, `[[Note#`
  for another note and `[[#` for the note being edited (read from the editor,
  so a heading typed a moment ago is there). Picking one inserts
  `Note#Heading` and closes the brackets. A heading holding `# | ^ [ ] :`
  or `\` is inserted with those as spaces, as Obsidian does. A link's
  heading is matched with all punctuation ignored, as in Obsidian, so
  `[[Note#Part 1: Intro!]]` opens `## Part 1: Intro?` and a link Obsidian
  wrote with punctuation kept opens its heading too. A nested
  `[[Note#H1#H2]]` names the H2 inside H1's section (H1 up to the next
  heading of its level or higher), in the editor, a page preview and a
  published page; after `[[Note#H1#` the popup lists the headings in that
  section.
- **Block ids.** `[[Note#^` and `[[#^` list the `^id` markers the same way.
  Obsidian's search over every block (`[[^^`) is not offered.
- **Aliases.** A note's frontmatter `aliases:` appear in the name list as
  "alias -> Note"; picking one inserts `[[Note|alias]]`, which shows the alias
  and links to the note. When another note has the same name it inserts the
  path (`[[folder/Note|alias]]`) so the link is not ambiguous. An alias is not
  also listed as a plain name.
- **Embeds** (`![[`) behave identically.

Another note's headings or block ids come from the index, with one small query
the first time the popup shows them and none per keystroke after that. Nothing
is queried while the popup is closed.

## Autosave

With `files.autosave` on (the default) every modified note is saved two
seconds after typing stops - or 30 seconds after the first edit since the last
save, when the typing never stops that long - and leaving a note, closing a tab
or quitting saves it too; nothing asks. A save that fails (disk full, read-only folder) says so in
the status bar and is tried again at the next pause, with no further edit
needed. If leaving or quitting then still cannot save, the usual
Save / Discard / Cancel box appears with the error in its text, so a window is
never refused a close without a word. With the setting off, the box is always
the way out and nothing is saved on a timer.

A note that is not valid UTF-8 (a stray Latin-1 byte, say) opens with the bad
bytes shown as U+FFFD and a status-bar line saying so. It is never saved
automatically, on a timer or on leaving, because that would write U+FFFD over
bytes nobody edited: the status bar says so instead, and leaving it with edits
asks (Save / Discard / Cancel). Ctrl+S asks "Save anyway?"; Yes replaces the
invalid bytes and from then on the note saves like any other. Nothing else
writes such a note either: `Vault::write` refuses a `Note::lossy` one, which
also stops link updates on rename and merge from rewriting it (merge refuses a
source like that outright).

The index (and so search) reads such a note's bad bytes as the Latin-1 letters
they most likely were, so searching "café" finds a note saved as Latin-1; the
file and the editor still show it as it is.

## Undo across edits from elsewhere

An edit another tab on the same note (or another person, see SHARE.md) makes
reaches this editor as `applyExternalEdit`. Undo and redo keep working across
it: the history is whole-text snapshots, and each is carried over the edit - one
region of the snapshot differs from the text now (`MarkdownEditor::hunkBetween`,
head and tail, never between the halves of a surrogate pair), and the outside
edit lands before it or after it, so it is made in the snapshot too. Undoing
here then undoes only what was typed here. An edit that lands inside that
region drops that step and every older one (they hold the region too); one that
replaces the whole text empties the history, as an undo in another tab does.
Only the newest 100 steps are carried (each costs a scan of the note).
`ExternalEditTest.*`, `HunkTest.*`.

## Deleting

Settings > Files > "Deleted files" (`files.deletedFiles`, read from
`trashOption` in `.obsidian/app.json`) is Obsidian's choice: `system` (the
default; the desktop trash, freedesktop `$XDG_DATA_HOME/Trash`), `local` (the
vault's `.trash`, `trashOption` "local") or `permanent` (`trashOption` "none").
It applies to the file tree's Delete and to the source of a merge. When there is
no system trash to use, the file goes to the vault's `.trash` and the status bar
says so; a failed trash is never turned into a permanent delete. Sync pulls
that remove a note keep using the vault's `.trash`.

## Windows and splits

Tabs sit in splits: "Split right" (`workspace:split-vertical`) and "Split down"
(`workspace:split-horizontal`) open the same note beside the focused tab, and
closing a split's last tab closes the split. Right-clicking a tab lists Close,
Close others, the two splits and the two window commands below.

A tab can also live in a window of its own, as in Obsidian's pop-out windows.
"Open current tab in new window" (`workspace:open-in-new-window`) shows the
note again in a new window; "Move current tab to new window"
(`workspace:move-to-new-window`) takes the tab itself, with its history, caret
and view (a canvas or a PDF comes along). Both are rebindable like any command.
A pop-out holds tabs and splits like the main window, has no sidebars, header
or status bar, and is titled by its current tab. Every hotkey works in it, and
there is still one vault, one index and one set of side panes: the right
sidebar and the status bar follow whichever tab has the focus, in whichever
window it is. Dialogs, the settings page and the graph open in the main window.

- Moving the main window's only tab out leaves an empty "New tab" behind: the
  main window never has no tab.
- Closing a pop-out closes its tabs like any other close: a note is saved
  first (with `files.autosave` on; a failed save or a declined prompt keeps the
  window open) and Ctrl+Shift+T brings the last one back in the main window.
  A pop-out whose last tab goes closes with it.
- Closing the main window closes every pop-out with it, after saving the
  layout: the pop-outs, with their splits, tabs, size and the active tab, are
  written in Obsidian's `floating` node and come back at the next start. A
  window is sized, never moved (the compositor decides where it goes), and a
  background tab is read when it is first shown. A `floating` node written by
  Obsidian is read the same way; a tab of a view type Notolith does not have
  is skipped, and a window with nothing left to show is not made. With the
  feature off, `floating` is ignored. Without pop-outs the layout has no
  `floating` node.
- **Core plugins > Pop-out windows** (`plugins.popoutWindows`, Notolith's own
  switch; Obsidian has none) turns the feature off: the four commands and the
  menu items stop, and every tab in a pop-out returns to the main window, every
  detached sidebar and pane to its place. Off, no window object, drop target or
  timer exists.

A sidebar can leave the window as well. "Detach left sidebar"
(`app:detach-left-sidebar`) and "Detach right sidebar" (`app:detach-right-sidebar`)
are Notolith's own commands (Obsidian only drags): they put the whole sidebar in
a window of its own, the same entry as "Detach sidebar" in the context menu of
the left header or the right tab row. Right-clicking one of the right sidebar's
tabs also offers "Detach pane", which takes that pane alone and hides its tab;
the other panes stay. The detached sidebar or pane keeps working: the file
tree, backlinks and the rest still follow the active note. Closing its window
puts it back in the splitter where it was. While a sidebar is in a window,
"Toggle left sidebar" and "Toggle right sidebar" leave it alone, a narrow main
window does not fold it, and asking for a detached pane (the status bar's
backlink count, Search) raises its window instead of switching the sidebar.
Panes are not tabs here, so unlike Obsidian a sidebar view cannot be dragged
into the main area. Closing the main window, or turning the feature off, docks
them first.

A detached sidebar or pane is remembered like a pop-out of tabs: it is written
in the layout's `floating` node as a window holding one tab whose view type is
Obsidian's name for it (`file-explorer` for the left sidebar; `search`,
`backlink`, `tag`, `file-properties`, `outgoing-link`, `outline`, `footnotes`,
`bookmarks` and `localgraph` for the panes), and comes back detached at the next
start or when that layout is loaded. Notolith's own views have their own names
(`right-sidebar` for the whole right sidebar, `meta-notes`). A type Notolith does
not know, or a pane whose plugin is off, is skipped. Loading a layout first
puts every detached sidebar and pane back, so only the ones it names stay out.

Dragging does the same without a command. Drag a tab out of its tab strip (a
drag inside the strip still reorders), and while it is over a split a
translucent zone shows where it will land: the outer quarter of a split's
body along an edge splits there (left or right: side by side; top or bottom:
stacked) and moves the tab into the new split, the rest of the body joins the
tab to that split, and a drop on a tab strip puts it at the tab under the
pointer. This works between windows too. Let go outside every Notolith window
and the tab opens in a window of its own, as "Move current tab to new window"
does; let go over a sidebar or the header and nothing happens. Dropping a
split's only tab on its own edge does nothing, there being nothing to split
from. A drop only ever moves a tab: no tab is made or lost, and a note open
in it keeps its place in history, caret and view. The Pop-out windows switch
turns all of this off, drop targets included.

Escape cancels a drag: a tab or a pane let go that
way opens no window and stays where it was. (Qt reports an Escape-cancelled drag
like one let go outside every window, so Notolith watches for the key itself,
only while one of its drags runs.)

A tab of the right sidebar can be dragged too: pulled off its row and let go
outside every Notolith window, its pane opens in a window of its own, as "Detach
pane" does; let go over any window (the sidebar, the editor, a split's edge) and
nothing happens. Panes are not tabs, so no split takes one. The left sidebar
has no tabs to drag; "Detach left sidebar" takes it whole.

The tests send the drag-enter and drop events themselves, because the offscreen
platform has no drag of its own (a started one ends at once as a drop outside
every window, which is how the pane drag-out is tested): they cover the zone arithmetic, the drop on an
edge, on the centre, on foreign data and with the feature off. Not covered, and
to try by hand once on a real display: the drag itself (`QDrag::exec`, its
pixmap), the synthetic release that ends the tab bar's own move when a drag
leaves it, dragging between two real windows, how the overlay looks, the
drop outside every window of a tab (which is `newPopout`, tested under pop-out
windows), and Escape during a real drag (the key's path ahead of Qt's own drag
filter is tested; that Qt delivers the key there is not).

## Undo close tab

Ctrl+Shift+T (`workspace:undo-close-pane`, rebindable like any command) reopens
the tab closed last, in the split it was in (the focused one if that split is
gone), with the caret and scroll it had. The last 20 closed tabs are kept for
the session; a note deleted since, or an encrypted one while the notes are
locked, is passed over, and an empty tab is not remembered.

## Status bar counts

Words and characters in the bar skip the frontmatter (the same text the word
count already skipped). With a selection the bar shows the selection's
instead, "N words, M characters", as Obsidian does. The note's counts still
come with each parse; the selection's are recounted 200 ms after the caret
last moved (`MainWindow::m_selectionCountTimer`), never on the keystroke or drag
path, and cost one pass over the selected text.

Two settings change what is counted (Settings > Editor), words and characters
alike, the selection's included:

- **Exclude text between characters** (`editor.countExcludeBetween`, off):
  everything between an opening and a closing delimiter, delimiters included,
  is left out. The delimiters are `editor.countExcludeOpen` / `Close`, 1 to 3
  characters each (any, Markdown's own too); empty means the meta note
  delimiters. Runs are found as meta notes are, so a link's or wikilink's
  brackets are not a pair.
- **Ignore formatting in counts** (`editor.countIgnoreFormatting`, off): only
  rendered words count. Left out: what live preview hides (heading, quote and
  list markers, link and wikilink targets and brackets, fences and language
  tags, footnote labels), a list item's number and task box, a callout's
  `[!type]` marker, every HTML tag (its text still counts) and `%%` comments.
  A comment's text does not count either: it is not rendered, and counting the
  words of what the note hides would be the surprise. Code and maths content
  still counts.

The parser does the sums: `Metadata::wordCount` follows the options, and
`Decorations::uncounted` holds the left-out ranges (sorted, disjoint, after the
frontmatter) that the editor takes off the character count and the selection's.
Both off, the list is empty and the count is the plain one. Tests:
`WordCountTest` in `tests/test_markdown.cpp`, `CountTest` in `tests/test_editor.cpp`.

## Bare brackets

A `[text]` that is not a link is drawn in `--bracket-color`, which is the link
colour (`--text-accent`) unless a theme says otherwise; delimiters kept, in
Live preview, Source and Reading. It is ordinary syntax colouring, on for
everyone, with no setting. It is not one when it is a wikilink, an embed,
`[text](url)`, `![alt](img)`, a reference link (`[text][ref]`, `[text][]`, or a
shortcut `[text]` whose `[text]: url` definition exists - md4qt decides), a link
definition, a footnote (`[^1]`, `^[inline]`), a task box at an item's start or
a callout marker; nor inside code, maths, HTML, the frontmatter, `%%` comments
or a bare URL; nor after a backslash. Brackets nest (`[a [b] c]` is one run, the
outermost), a run may cross a soft line break but not a blank line, a line that
starts another block, or a table cell, and an unclosed `[` is not a run.

The runs come from one linear pass (`scanRuns` in `libs/markdown/Parser.cpp`),
asked for through `ParseOptions::brackets` - the editor does, the indexer and
publisher do not, so they pay nothing. Output: `StyleSpan::Kind::BareBracket`.

## Meta notes ([...])

Setting `editor.metaNotes` (Settings > Editor, **off** by default). A writer's
notes to themselves, such as `She left early [check the date] and never came
back.`, are drawn in their own colour, `--meta-note-color` (orange unless a
theme sets it), delimiters kept. They stay visible while you write - nothing
hides them, and there is no switch that would. The delimiters are
`editor.metaNoteOpen` / `editor.metaNoteClose`, default `[` and `]`; each is 1
to 3 characters, and the same string on both sides (`;;`) pairs them left to
right. Characters Markdown already
owns are refused with a message in the settings page, not guessed at: `* _ ~ =`
backtick `$ % # |` (`metaNoteDelimitersError`). A pair the settings file holds
that fails the check falls back to the brackets.

Matching is the bare bracket scan with the delimiters as parameters. With the
brackets all the exclusions above apply; with any other pair only the generic
ones do - code, maths, HTML, links, wikilinks, footnotes, comments, the
frontmatter and URLs are never looked inside. With the brackets a run is a meta
note instead of a bare bracket (the meta colour wins); with another pair both
are drawn, a bracket inside a note keeping the later span.

Off, `ParseOptions::metaNotes` is false and the parse never looks. On, each run is
a `StyleSpan::Kind::MetaNote` and the ranges are in `Decorations::metaNotes`
(`MarkdownEditor::metaNoteRanges()`), outermost, delimiters included, in source
order - what the meta pane and the jump build on. Tests:
`MetaNoteTest` and `BareBracketTest` in `tests/test_markdown.cpp`,
`BracketStyleTest` and `MetaNoteStyleTest` in `tests/test_editor.cpp`.

**Jump.** `Alt+]` / `Alt+[` (commands `noteAppC:next-meta-note` /
`noteAppC:previous-meta-note`) select the next or previous run after the
selection, delimiters included, so Backspace deletes it in one stroke, an arrow
key puts the caret inside to edit it and typing replaces it. Past the last run
it wraps to the first. `MarkdownEditor::selectMetaNote(direction, &index, &count)`
is linear over the run list and never parses; it works in every mode. A caret on
the opening delimiter selects that run, not the one after it.

**Delete.** Right-click inside a run: "Delete meta note" removes it and one
neighbouring space (`early [x] and` -> `early and`, `[y] She` -> `She`,
`back [z].` -> `back.`), as one undo step. This is the only way a note leaves the
text; it is never hidden. Tests: `MetaNoteJumpTest` and `MetaNoteContextMenuTest`
in `tests/test_editor.cpp`.

**Pane.** While `editor.metaNotes` is on, the right sidebar has a Meta notes tab
(`MetaNotesPane`, `libs/ui`, a brackets icon after Outline); off, the pane, its
tab, the two commands (disabled, so absent from the palette) and the status count
do not exist - nothing is built. It lists the note's runs in document order,
grouped under the nearest heading above each (flat in a note without headings;
a heading with none is not listed): the run's text, then a muted line with its
line number and the words around it, the run in `--meta-note-color`. A long row
is clipped, not wrapped, so a row is always two lines; the tooltip has the whole
run. The run the caret is in is highlighted; Return or a click selects it in the
editor. The filter box narrows the rows by their text. `MainWindow` feeds it the
editor's `noteText()`, `metaNoteRanges()` and `headings()` on every parse;
`setNote` is a no-op when every row would read the same, and when only offsets
moved (typing above a run) it rewrites them in place and rebuilds nothing.
The status bar shows "N meta notes" (click: opens the pane), hidden at zero.
`Alt+]` / `Alt+[` report "Meta note 2 of 3" there, "(wrapped around)" past either
end and "No meta notes in this note" with none. Changing the delimiters in
Settings refreshes the pane and the count from the next parse.

**Vault mode.** The Vault toggle beside the filter swaps the list for the notes
of the whole vault that have meta notes: an embedded `SearchPane` (built on the
first press, with its own object names `metaVaultSearch` / `metaVaultQuery` /
`metaVaultList`, query box hidden) running `meta:` - or `meta:(<filter>)` when
the filter box has text, after the usual 150 ms - through `Index::searchIn`, so
it works with Global search off. One row per note with its run count and a
snippet; picking one opens the note at its first run and flips the pane back to
"This note" (vault -> note -> run). Deliberate ceiling: one snippet per note,
every run only after the drill-down. `MainWindow::applyFeatures` passes
`parseOptions()` to `Indexer::setMetaNotes`, so the index holds the runs only
while the setting is on (see SEARCH.md, `meta:`). Tests: 
`WindowTest.TheMetaNotesPane*`, `AltBracketJumps*`, `ChangingTheDelimiters*`
(vault mode: `TheMetaNotesPaneVaultMode*`).
Fixture: `demo/Editor/Meta Notes.md`; to see the pane in
`noteAppC_shot`, put `[editor]` / `meta-notes = true` in a private
`$XDG_CONFIG_HOME/noteAppC/settings.ini` and set `NOTEAPPC_SHOT_KEEP_CONFIG=1`.

## Fenced code and the caret

The caret anywhere from a fence's opening line to its closing line, not only on
a fence line, shows both fence lines and the language tag (Obsidian treats the
block as one unit), and moving out hides them again. md4qt's code block
covers the listing alone, so `fenceAroundCaret()` marks the blocks either side
as active and `revealedSyntax` reveals their fence markers. A listing inside a
callout wraps in live preview, as Obsidian's does (a language-less listing
there scrolls on a single unbroken token; here it is broken, which no
reader has asked about).

A fence outside a callout never wraps: its longest line scrolls sideways inside
the panel. That is Obsidian's reading-view rule, chosen over its live-preview
rule (CodeMirror wraps there) because a wrapped diff hides where its lines end;
`#| wrap: true` (Quarto's `code-overflow: wrap`, whose default is `scroll`) opts a
fence back into wrapping, and source mode wraps, having no panel to scroll in.
Indented code is a fence for this. It reuses the wide-table mechanism:
`BlockView::sideWidth` (the widest line, 0 while everything fits) and
`sideScroll`, one offset per fence held by its first piece, so every piece
moves together (`sideExtent` is the widest of them, `sideOffset` the clamped
offset; a piece not laid out yet has no width, so the extent can grow as the
fence scrolls into view). A horizontal wheel, Shift plus the vertical wheel
and a trackpad's horizontal pixels pan it (`sidewaysScrollableAt`); the text is
clipped to the window right of the `#| lines` gutter, whose numbers stay put,
and `paintSidewaysOverflow` draws the edge fades and the 3 px bar the tables
use. `pointFor`, `offsetAt` and `ensureCursorVisible` carry the offset, so the
caret, a click and a drag land where the characters are and typing, End, Shift+
arrows and vim's `$` keep the caret inside the window. The rebuild after every
parse copies the offset to the new first piece. A fence with no overlong line
pays nothing. Not done: dragging a selection past the right edge does not
autoscroll (a table's `m_tableScrollTimer` does), and there is no draggable
thumb.

Every fence with a language is laid out under a header strip (`hasCodeHeader`,
one `codeHeaderHeight` tall) that names it (`codeTitleOf`: `#| title:`, else the
style's name; nothing in a tab group, whose tabs are the names). The copy button
(`codeLabelOf`) is at the right end and shows only on hover or just after a copy,
but its place is always reserved so the run control beside it never moves. The
run control exists only where `setNotebookRunnable`'s predicate says something
can run the language: the window answers with `DirectRunner::canRun` or a Jupyter
kernel spec, remembered per language for the session, so a block nobody can run
has no control, no output row and no "does not know how to run" warning.

A tab group's strip draws the shown tab as attached to its listing: a
rounded-topped tab in the panel's colour, edged on three sides, with the hairline
under the strip left out beneath it (`paintCodeHeader`; `selectedTabRectAt` is
its rect for the tests). `noteAppC_shot --caret <offset>` puts the caret in a
note to look at that edit state.

A tab group (adjacent fences marked `tab`, see STYLING.md) being edited is
plain source: with the caret anywhere in it every member shows its fence lines,
`#|` options and listing as text, no panel, tab strip, run control, gutter or
caption, and none of its blocks runs ("Run all blocks" skips them) until the
caret leaves. The listings keep their syntax colours. This is `pureSource()`,
the one test Source mode shares with it, and `BlockView::editedAsSource` already
re-lays the group out when the caret crosses its edge.

A single fenced block whose fences the caret has opened (`BlockView::editedListing`)
is plain text too - no header strip, copy label, `#|` rows, gutter or media
rendering - but it keeps its panel, its run control and its last output
(`pureSource()` is false for it, so the notebook eligibility flip and
`notebookOutputs.clear()` stay tab-group-only and Source-mode-only). The fence
lines are in the paragraphs either side of the listing, so the panel
(`editedListingPanel`) reaches out one line above and below to take them in, and
is drawn first in `paintBlocks`, before any text, so nothing is drawn over a
fence line. The run control moves to the panel's top-right corner, on the
opening fence line (`notebookRunRectFor`). The block loses only its header strip
when the caret enters: 31.5 px at the default font (`codeHeaderHeight`; nothing
for a listing with no header). The output rows stay directly under the listing,
so the closing fence line follows them.

## Source mode

Source mode draws the text and nothing rendered: markdown syntax in its own
colours, listings highlighted, line numbers, indent guides (a setting) and
heading sizes (text, as in Obsidian's source mode). No code panel, header, run
control or output, no callout panel, title bar or fold (a `[!x]-` callout shows
its body), no properties table, tables, embeds, maths, queries or bullets.
Headings and list parents still fold (arrow, "..." box and all). A
renderer that forgets to ask `plainSource()` shows up as a panel in Source mode.

## Vim key bindings

Settings > Editor > "Vim key bindings" (`editor.vimMode`, off by default).
Off, `keyPressEvent` pays one bool test and nothing else runs. There is no
CMake option, because there is no dependency to drop: the whole thing is
`libs/editor/VimMode.cpp` and a small state struct in `MarkdownEditor_p.h`.

No vim-emulation library is installed (FakeVim ships inside Qt Creator only),
and none is downloaded, so this is hand-rolled. It is a friend of
`MarkdownEditor` and edits the editor's own buffer, caret and undo stack.

The mode, a half-typed command (`2d`) and the `:` / `/` line show in the
status bar. Normal and visual mode draw a block caret; insert mode keeps the
editor's own bar.

### What is covered

| area | keys |
|---|---|
| modes | `Esc` (and `Ctrl+[`) to normal; `i a I A o O` to insert; `v` characterwise, `V` linewise and `Ctrl+v` block visual, `o` swaps the selection's ends |
| motions | `h j k l`, `w W b B e E`, `0 ^ $`, `gg G` (`5G` goes to line 5), `{ }`, `f F t T` with `;` and `,`, `n N`, `* #`, `%`, `'x` `` `x ``, `gj gk` (by drawn line, for wrapped paragraphs); arrows, Home and End also work |
| counts | before a command or a motion: `3w`, `d2w`, `2d3w`, `5x`, `3J`, `4rx` |
| operators | `d c y > <` with any motion, doubled for a line (`dd cc yy >> <<`, `3>>` is three lines), `D C Y S s x X`; in visual mode `d x y c s > <` (`3>` shifts three times) |
| text objects | after an operator, or in visual mode: `iw aw iW aW`, `is as`, `it at` (HTML/XML tags: nested, case-insensitive names, void and self-closing tags and comments skipped, attributes may hold `>` in quotes, works inside an HTML block; a count goes that many elements out), `ip ap` (lines), `i" a" i' a'` and ``i` a` `` (on the line), and brackets `i( a( ib ab`, `i[ a[`, `i{ a{ iB aB`, `i< a<` (the closing character names them too). A count means that many words, sentences or paragraphs, or that many bracket levels out. `i{` on a block whose braces sit on their own lines takes the lines between |
| `%` | jumps to the matching `()`, `[]` or `{}`: from the first bracket at or after the caret on the line; works across lines; usable after an operator (`d%`). With a count, `50%` goes to that percent of the file (linewise, `d50%` works, over 100 fails) |
| `* #` | search for the whole word under the caret, forward or back; `n` and `N` go on from there |
| indent | `>` and `<` move lines by the editor's tab width (Settings > Editor), `<` also takes one leading tab; empty lines stay empty |
| registers | `"a`-`"z` before a yank, delete, change or put (`"ayy`, `"ap`); `"A`-`"Z` append; `"0` is the last yank, `""` the unnamed one; `"1` is the newest delete or change of a line, of several lines, or ending in `% { } n N`, and each one shifts the older ones along to `"9`; `"-` is a smaller delete (within a line, no register named); `"_` is the black hole; `"+` and `"*` are the system clipboard (a put from it is lines if the text ends in a newline) |
| marks | `ma`-`mz` set; `'a` goes to the line and the backtick form to the exact place, both usable after an operator (`d'a`); `''` returns to where the last jump began (`G gg % n N * # { } :N` and mark jumps are jumps); marks follow edits; `'<` and `'>` are the last visual selection |
| macros | `qa` ... `q` records into `a` (`qA` appends), `@a` plays it, `@@` plays the last one again, `3@a` three times; the status bar shows `recording @a`. A macro is the keys as typed, so insert-mode typing replays too. One that runs itself stops after 8 levels |
| block visual | `Ctrl+v`, then motions (`$` runs every line to its end); `d x c y` act on the columns, `I` and `A` insert on the top line and copy the typed text to every line when `Esc` ends it (`I` skips lines too short to reach the block, `A` pads them), `>` `<` shift the lines; a yanked block is put back as a block |
| put and edit | `p P` (character, line or block, as yanked), `r<char>`, `~`, `J`; in visual mode (characterwise, linewise or block) `J` joins the selected lines (one line: with the next), `r<char>` replaces every selected character (line breaks stay), `~` toggles case, `u` lower-cases and `U` upper-cases the selection |
| undo | `u`, `Ctrl+r`; each command is one undo step and a whole insert session is one |
| repeat | `.` repeats the last change, including what was typed in its insert |
| search | `/pattern`, `?pattern`, `n`, `N`: a regular expression, case-insensitive unless it has a capital; shown with the editor's find highlight, cleared by `:noh` |
| ex line | `:w` saves, `:q` closes the tab (refused with unsaved changes), `:q!`, `:wq`, `:x`, `:N` goes to line N (any address: `:$`, `:.+3`, `:'a`), `:noh` |
| substitute | `:s/pat/rep/[g][i]` on the line, `:%s/...` on every line, `:3,5s`, `:.,+2s`, `:2,$s`, `:'a,'bs`, and `:'<,'>s` (typing `:` in visual mode fills that in). `pat` is a regular expression (Qt's, as in `/`), the delimiter can be any punctuation, `rep` has `&`, `\1`-`\9`, `\r` (new line), `\t`. Case-sensitive unless `i` |

Notes on behaviour that differs from a terminal vim:

- `j`/`k` move by source line, as in vim; a long paragraph is one line. Use
  `gj`/`gk`, or the arrow keys, to move by drawn line.
- The unnamed register is the editor's own; yanking does not touch the system
  clipboard, so `Ctrl+C` / `Ctrl+V` keep working beside it. In visual mode
  `Ctrl+C` copies the visual selection.
- In normal and visual mode a Ctrl chord that vim mode does not own
  (`Ctrl+F`, `Ctrl+S`, `Ctrl+B` ...) still reaches the editor's normal
  shortcuts. In insert mode every key but `Esc` does: typing, IME,
  auto-pairs, the slash menu and `[[` suggestions are the editor's own.
- Composed (IME) text is ignored outside insert mode; touch and mouse
  selection work as before, and a mouse click while in visual mode drops
  the visual selection.
- With vim mode on, the caret's block always shows its source, also while
  something is selected.

### Not covered

`:set`, a count on `.`, `p` over a visual selection, a text object that
extends an existing visual selection (it replaces it), `gu`/`gU`/`g~`, `U` as
line undo, and `:s` flags other than `g` and `i`. A macro stops at its first
failed motion (`j` on the last line, an `f` that finds nothing, a search with
no match, `l` on a line's last character or an empty line, `h` at column 0).
Block columns count characters, not drawn width, so
a tab is one column. In normal and visual mode `Ctrl+v` is block-visual, not
paste; `Ctrl+V` still pastes in insert mode. Visual mode highlights the
character under the caret by widening the selection by one, which is also why
`Ctrl+C` copies exactly what is highlighted (for a block selection that is the
span between its corners).

### Tests

`tests/test_vim.cpp` drives a real `MarkdownEditor` with key events (motions,
operators, text objects, registers, marks, macros, block-visual, `:s`, counts,
undo, mode switches, search, the ex line, IME, the caret);
`tests/test_window.cpp` covers the setting, the status bar and `:w`.

## Pipe tables as source

A table is edited in its cells in live preview (`TableCells.cpp`) and as text in
source mode; both keep the file reading as aligned ASCII art. All of it is one
undo step per action and is switched off by `editor.alignTables`, except the
commands, which are always there.

- **Typing one.** `| a | b |` then Enter writes the delimiter row and an empty
  body row under it, the whole table padded (`| a   | b   |`, `| --- | --- |`,
  `|     |     |`), and puts the caret in the first body cell. It happens only
  when the line is a finished row (caret past the closing pipe), the line above
  has no pipe, the line below is blank, the line is not in a code fence or a
  delimiter row itself, and has a cell with text. Anywhere else Enter is the
  newline it always was, so nothing is injected into an existing table. Cells are
  counted by the parser's `tableRowCells`, so `\|` is not a border (a pipe in a
  code span is one, as in GFM).
- **Insert table** (`noteAppC:insert-table`; `/table` and `/insert table` in the slash popup both put it first: a typed word that is a whole word of a command's name outranks fuzzy matches, shortest name first):
  Obsidian's 2x2 template, the caret in its first cell; on a line with text the
  table goes below as a paragraph of its own.
- **Tab, Shift+Tab, Enter in source mode** inside a table align that table (not
  per keystroke: it would move the caret's column under the typist) and move to
  the next cell, the previous one, or the cell below; past the last row they add
  one. Other tables are untouched byte for byte. Rendered cells already did the
  moving; they align when the caret leaves, as before.
- **Commands** (palette and the cell's right-click menu): `Table: Align column
  left/center/right` (rewrites the delimiter cell), `Duplicate row`, `Duplicate
  column`, `Sort column ascending/descending` (numbers by value, header and
  delimiter row fixed, equal cells keep their order).

Tests: `TableAutoTest.*` (`tests/test_tables.cpp`).

## Touch

Only where a touchscreen exists (`grabTouchScroll`, PopupStyle.cpp), so a
desktop with a mouse keeps drag-to-select:

- A finger dragged on a note scrolls it, with momentum, and selects nothing;
  the page starts moving after 2 mm of travel (Qt's default is 5 mm, which
  read as the page sticking). A held finger (500 ms) selects the word under
  it, and that selection gets handles a finger drags to move its ends.
- Every outermost scroll area does the same: the settings page, the file
  tree, the panes. One inside another (a list in a block, a block in a note)
  leaves the drag to the one around it, so a drag that starts on it still
  scrolls the page.
- The OpenGL viewport is a new widget, so turning it on grabs the gesture
  again on that one.

GPU acceleration (`editor.gpuAcceleration`, Settings, Editor) is Automatic by
default: OpenGL on a screen with a fractional scale, where raster has to
repaint the whole note on every scroll step, and raster elsewhere, where it is
faster. On and Off force it; a settings file from before Automatic keeps its
`true` or `false`. The numbers are in [PERFORMANCE.md](PERFORMANCE.md), "Scrolling and GPU acceleration".

Tests: `tests/test_touch.cpp` (its own binary: a test touchscreen is
registered for the whole process).

## Table handles and cell selection

What Obsidian puts around a rendered table, in `libs/editor/TableHandles.cpp`.
Pipe tables in live preview, and an HTML `<table>` that is a simple grid (below);
a base keeps what it had. Every pipe-table action rewrites the source through the same row/column model the
table commands use (`loadTableText`, `storeTableText`, then `formatTable`), so it
is one undo step and the table comes out aligned. Behaviour only is taken from
Obsidian; none of its code.

- **Grips.** Pointer over a table: a small pill straddles its left edge at the
  hovered row and its top edge over the hovered column (accent when the pointer
  is on it, open-hand cursor). Drag a row grip up or down, a column grip
  sideways: a bar shows where it would land and the drop reorders the source.
  The header row is fixed (its grip only selects, nothing can be dropped above
  the first body row; an HTML table's header is as many rows as its `<thead>` or
  leading `<th>` row says), the delimiter row stays second, and a column's alignment
  moves with it. A click on a grip without moving selects that row or column.
- **Add bars.** A thin strip with a "+" under the table and another right of
  it (below the `</>` button) appends an empty padded row or column on a
  click; the caret goes into its first cell, so typing fills it. A new column
  gets a `---` delimiter cell. Dragging a bar adds one row (down) or column
  (right) for each row height or column width travelled, previewed as a tinted
  strip while dragging; the table changes on release, as one undo step. A click,
  or a wobble of a few pixels, adds one.
- **Cell selection.** Drag from one cell into another (or Shift+Up/Down, and
  Shift+Left/Right at a cell's edge) selects a rectangle of cells, drawn as an
  accent tint and outline; the caret stays in the anchor cell. Dragging back
  into the first cell, or out of the table, is the text selection it always
  was. Shift+arrows grow and shrink it, Escape drops it, any other key or click
  ends it, and it ends by itself when the text or caret changes.
  - Ctrl+C: the cells as a pipe table (first selected row is the header, the
    columns' alignment goes along) as `text/markdown`, and as tab-separated
    values in `text/plain`, which is what a spreadsheet takes. Ctrl+X does that and empties
    the cells; Delete or Backspace empties them. A copy writes nothing; a cut
    or a clear is one undo step.
  - Ctrl+V: a pipe table (`text/markdown` first, else the text; its delimiter row left out) or tab-separated values
    goes into the cells from the top-left of the selection, or from the caret's
    cell when none, and adds the rows and columns it needs. One value over a
    selection fills every cell of it. What was pasted is left selected. A word
    or a line with no selection is the ordinary paste (Ctrl+Shift+V too).

Setting `editor.tableHandles` (Settings > Editor, on by default). Off: no hover
tracking for tables, no grips or bars, and a drag across cells selects text.
Hover is a mouse-move path like the `</>` button's: two binary searches (block,
cell), a repaint of only the grip or bar that changed, nothing laid out. The
`</>` button wins where they overlap. Nothing runs on a keystroke but one
comparison (and a clipboard read on Ctrl+V).

- **Autoscroll.** A grip, an add bar or a cell selection dragged past the edge
  of the viewport scrolls the page, and past the edge of a wide table's window
  pans the table, on a 16 ms timer (`m_tableScrollTimer`) that runs only during
  such a drag: 2 px a tick at the edge, 1 px more per 4 px past it, at most 40.
  A cell-selection drag past those edges stays a rectangle instead of falling
  back to a text selection. Release (or the handles setting off) stops it. The
  caret's move to the anchor cell no longer scrolls the page back
  (`setCellSelection` keeps the scroll).
- A table laid out as source for a moment (an edit that rewrites it leaves the
  caret outside its cells until the caret is placed) is invalidated again when
  the caret lands in a cell (`moveCursor` no longer skips a table block for
  having no cells), so a grip drop, a bar or a clear leaves a grid, not source.
- **Touch.** A finger has no hover, so its first tap in a table brings the grips
  and bars up for the row and column it is on (the move Qt sends ahead of a
  touch press does not, `updateHover(point, false)`); the next press on one of
  them is theirs and the finger drags it (`QScroller` is stopped, the hit
  rects grow by the touch slop). A tap on a grip selects, on a bar adds one,
  a drag does what the mouse does. A long press in a cell selects its word as
  ever and arms a rectangle: the finger, still down, now drags across cells.
  A finger that moves at once is a scroll, as before. A second tap that comes
  within the double-click interval reaches the editor as a double click, which
  forwards it to the same press. Tests: `TouchTableTest.*`
  (`tests/test_touch.cpp`, with `QTest::touchEvent`).
- **HTML tables.** A `<table>` the editor draws as a grid (HtmlEdit.cpp) gets the
  same grips, bars and rectangle when it is *simple*: no `rowspan` or `colspan`
  other than 1, and every row with as many cells as the widest
  (`TableSpan::simple`); any other gets none and is edited cell by cell as
  before. A reorder permutes the rows' (or each row's cells') own text between
  their slots, so attributes, `<th>` vs `<td>`, the author's indentation, line
  breaks and comments between rows, and every untouched row stay byte for byte;
  adding appends rows written like the last one, or a cell after each row's
  last. One undo step each. Ctrl+C copies what the cells show (tags and
  entities resolved, one line each) as TSV in `text/plain` and a pipe table in
  `text/markdown`; Delete or Ctrl+X empties the cell contents and keeps their
  tags, the spaces inside going too unless they span lines (then the line
  layout stays and only the text goes). Paste fills from the top-left cell as
  in a pipe table, text HTML-escaped; the table grows with cells and rows
  copying its own last cell's tag, the gap between cells and the last row's
  layout, th only in the header. One undo step. A table with a merged cell
  keeps the ordinary paste.

Tests: `TableHandlesTest.*` (`tests/test_tables.cpp`), `HtmlTableHandlesTest.*`
(`tests/test_html_edit.cpp`).

## Diagrams: Mermaid, PlantUML, TikZ, Graphviz (and Markmap)

A ```` ```mermaid ```` fence is drawn as its diagram when the caret is outside
it, and shown as source when the caret is inside, like any rendered block.
Settings > Core plugins: "Mermaid diagrams" (`render.mermaid`, on by default)
and "Mermaid tool" (`render.mermaidTool`, default `mmdr`).

The drawing is done by an external program, not linked in: `mmdr`, the
pure-Rust `mermaid-rs-renderer` crate (installed with
`cargo install mermaid-rs-renderer`; 1-5 ms a diagram, about 300 ms for the very
first run of a cold binary). noteAppC installs and fetches nothing, and there is
no CMake option: no compiled-in code depends on it (Qt6::Svg draws the result,
already linked for notebook output).

- **Startup cost**: reading one bool. Nothing is resolved, spawned or warmed
  until a mermaid fence is laid out with the setting on. Off, no renderer is
  registered and a fence is an ordinary code block.
- **Off the UI thread**: `libs/editor/MermaidBlock.cpp` is a renderer for
  `MarkdownEditor::setCodeBlockRenderer`, so the fence, its placeholder and its
  in-memory cache (by width, theme and source) are `CodeBlockHook.cpp`'s. A job
  runs `mmdr -i - -c config.json` on the kind's pool of two threads (a hung tool
  holds one, so the next fence still draws; source on stdin, SVG on
  stdout; `-o -` is not used, 0.3.1 writes a file named `-` for it) and
  rasterises the SVG with `QSvgRenderer` at the column width and the display's
  pixel ratio. The tool gets 5 s, then is killed.
- **Sandbox**: a fence can come from a shared note, so where bubblewrap is
  installed mmdr runs in the same sandbox as the other diagram tools (see
  "Sandbox" under PlantUML, TikZ and Graphviz below), with the 5 s budget and
  the 4 GB address-space limit; the config goes in the temporary directory,
  the one writable place. Unlike the other tools it is not refused without
  bwrap: mmdr only parses text, comparable to the poppler and QSvg parsers
  this process runs, and Mermaid is on by default, so it runs as a plain
  process there and the diagram still draws.
- **Cache**: `libs/render/MermaidCache.{h,cpp}` (in libs/render because publish
  does not link the editor). The SVG is kept as
  `$XDG_CACHE_HOME/noteAppC/mermaid/<hash of source>-<hash of config>.svg`, and in
  memory. A cached diagram never starts the tool again. The first diagram made
  or read in a process, once it has been delivered and never at startup, prunes
  the directory on the render thread: files not used for 30 days go, then the
  least recently used until 50 MB is left (`pruneMermaidCache`). A disk hit
  counts as a use by touching the file's modification time.
- **Colours**: the config handed to the tool is Mermaid's own shape (`theme`,
  `themeVariables`) in the app's colours. mmdr 0.3.1 honours it: the colours
  land in the SVG, the background included (`theme` alone equals `-t`), so there
  is no retry without the config and no re-colouring.
- **No tool, or a failure**: the source shows in a listing with one dim line
  under it ("Mermaid: install mmdr to render", or the tool's first error
  line). A missing tool is remembered until the setting changes, so typing
  does not look for it again; a failure is cached with the source.
- **Tool lookup**: a bare name is searched on `PATH`, then in `$CARGO_HOME/bin`,
  `~/.cargo/bin` and `~/.local/share/cargo/bin` (a desktop launcher's `PATH`
  often lacks them); anything with a `/` is a path.
- **Publish and PDF export** never start the tool: they use the SVG the editor
  already cached for that source (the theme's colours if it has them, else the
  most recently used in any colours - the diagram carries its own background, so
  a dark one is a dark card on a light page). Publish embeds it as
  `<div class="diagram"><img src="data:image/svg+xml...">`, never inlined (an
  SVG drawn as an image runs no script), with the fence's source as the alt
  text (escaped, first 2000 characters) for screen readers and the site's
  search; PDF export
  draws it as an image. A fence never drawn in the editor is its source, as a
  code block.
- **Not drawn**: encrypted notes (the source would go into the cache), slides.

`tests/test_mermaid.cpp` runs all of this against a fake `mmdr` script (with
bwrap hidden from `PATH`, as the fake keeps its books outside its directory;
the sandbox tests put it back: the tool gets its input there, cannot read the
home or write outside its directory, and still draws without bwrap), plus
one test against the installed tool, run in the sandbox (skipped when absent, a failure under
`NOTEAPPC_REQUIRE_TOOLS=1`, which the full check run sets); the publish test is in
`tests/test_publish.cpp`.

### PlantUML, TikZ and Graphviz

````plantuml` (also `puml`), ````tikz` and ````dot` (also `graphviz`) fences are
drawn the same way and by the same code (`diagramRenderer` in
`libs/editor/MermaidBlock.cpp`; `mermaidRenderer` is its Mermaid case): the one
pool (one per kind, two threads), the tool found by the first fence and remembered, the memo, the
disk cache (one directory per kind: `.../noteAppC/{mermaid,plantuml,tikz,dot}`),
the source with one dim hint when the tool is missing or fails, nothing at
startup, a cached diagram drawn without starting a process, and the caret
inside the fence showing its source. Settings, next to Mermaid's, each on by
default and costing nothing when off: "PlantUML diagrams" (`render.plantuml`,
tool `render.plantumlTool` = `plantuml`), "TikZ diagrams" (`render.tikz`, tool
`render.latexTool` = `latex`; `dvisvgm` is looked for next to it, then on
`PATH`), "Graphviz diagrams" (`render.graphviz`, tool `render.dotTool` = `dot`).
These need PlantUML (and Java), TeX Live's latex and dvisvgm, and Graphviz
installed; noteAppC bundles none (no C/C++ renderer exists for PlantUML or TikZ,
and Graphviz's libgvc was left out on purpose: a C parser with a CVE history
reading a shared note's graph inside the editor process, with its unsaved text,
is the wrong place for it).

- **Sandbox**: each of these runs inside bubblewrap with the syscall filter
  (`sandbox::bubblewrapCommand`, `libs/sandbox/Bubblewrap.h`, which TeX's
  compile step uses too): `/usr`, `/var/lib/texmf` and, from `/etc`, only `texmf`, `fonts`,
  `ld.so.cache`, `alternatives` and the JVM's `java*` directory (each if it exists) read-only, no home,
  no other disk, no network, no inherited environment, and one temporary
  directory writable. Without bwrap there is no diagram, only the hint: never an
  unsandboxed run (Mermaid alone is the exception, above). A tool set by path outside `/usr` is resolved (a symlink
  to the file it points to) and bound in read-only; whatever it needs besides
  itself (a wrapper script's jar, a home's libraries) is not, so install those
  under `/usr`. 20 s for a
  tool (and for each of TeX's two steps; Mermaid's 5 s), a CPU limit, and 4 GB of address space
  for all but the JVM, which reserves far more than it uses: it gets `JAVA_TOOL_OPTIONS`
  heap caps (`-Xmx512m`, metaspace 256 MB, direct memory 128 MB) and a 2 GB `RLIMIT_DATA` instead.
- **PlantUML**: `plantuml -tsvg -pipe -charset UTF-8` with the source on stdin
  (wrapped in `@startuml` when it has no `@start...`), `--dark-mode` on a dark
  theme, and `PLANTUML_SECURITY_PROFILE=SANDBOX`, so `!include` of a file or a
  URL fails (the error shows as the hint). A PlantUML without `--dark-mode`
  would fail the run; update it.
- **TikZ**: TikZJax's block (`\usepackage` / `\usetikzlibrary` lines, then
  `\begin{document}...\end{document}`) or a bare `tikzpicture`, put in a
  `standalone` document and run through `latex -no-shell-escape` and
  `dvisvgm --no-fonts --exact-bbox`. TeX can still `\input` a system file under
  `/usr` (TeX Live 2026's `openin_any` does nothing), but not the machine id,
  hostname or user list in `/etc`, which a shared note would otherwise put into
  a published SVG; the user's files are not in the sandbox either. On a dark theme black (explicit, and the SVG's implicit
  default) becomes the theme's text colour, like TikZJax's invert option; other
  colours stay. The SVG gets a background rect in the theme's background, so
  like Mermaid's and PlantUML's it is a card (not pale lines invisible on a
  light page) when publish or PDF export find it drawn for another theme.
- **Graphviz**: `dot -Tsvg`; the theme's text colour and background are declared at the top of the graph as the defaults, so the
  graph's own attributes override them.
- **Publish and PDF export** draw the cached SVGs of all four kinds the same
  way (a never-drawn fence stays its source); publish puts each in
  `<div class="diagram">` as an `<img>` with a `data:` URI, never
  inlined: a note decides what is in its SVG (TikZ's `\special{dvisvgm:raw}`
  passes markup straight through), and an SVG shown as an image runs no
  script and loads nothing.

`tests/test_diagrams.cpp` runs the real tools (skipped when one is absent, a
failure under `NOTEAPPC_REQUIRE_TOOLS=1`): a diagram per kind, the disk cache,
the missing-tool hint, PlantUML's refused includes, TikZ's reads and shell,
the colours, the sandbox itself, PDF export and the window's settings; publish
is in `tests/test_publish.cpp`.

### Markmap: mind maps without a web view

A ```` ```markmap ```` fence is drawn as a mind map the way the Obsidian markmap
plugin and markmap.js.org draw one: the root at the left, the fence's headings
and list items as a tree to its right, a curved branch in its own colour for each
subtree, a short underline under each node's text and, at its end, a circle
that folds the branch. Core plugin `markmap` (Settings > Core plugins, "Mind
maps", on by default); off, the fence is a plain code block and no code of it
runs. There is no JavaScript and no tool: the tree, the layout and the SVG are
`libs/render/Markmap.{h,cpp}`, drawn in-process, so reading view, publish and
PDF export show the same map, each drawn from the fence's text in its own theme.

- **Tree**: headings nest by level (a skipped level still hangs from the
  nearest lower one) and a list under a heading hangs from it, nested by
  indentation (tabs count 4); any other line of text is a child of the heading
  above it and a continuation line joins its list item. One top-level node is
  the root; several (or none) get an empty root, as in markmap.js. The node text's
  inline Markdown comes from the application's own parser (`Parser::parseAll`
  over the fence body, one flag per character): **bold**, *italic*, `code` (on a
  tint), links and wikilinks (their text, in the accent colour, underlined),
  ~~strikethrough~~; the markers are gone.
- **Options**: the fence's frontmatter, under a `markmap:` key or at the top
  level: `colorFreezeLevel` (nodes below this level share the colour of their
  ancestor on it; default 0, every node its own), `initialExpandLevel` (nodes on
  this level or deeper start folded; default -1, all open) and `maxWidth`
  (pixels before a node's text wraps; default 0, never). Levels count as
  markmap.js counts them, from 1 at the root: `initialExpandLevel: 1` folds the
  root (only it shows), 2 shows the root and its children, and
  `colorFreezeLevel: 1` gives the whole map one colour (markmap-view's
  `view.ts` `_initializeData` and `util.ts`, pinned by
  `MarkmapTreeTest.InitialExpandLevelCountsTheRootAsLevelOneLikeMarkmapView`). `<!--
  markmap: fold -->` after a node, or on a line of its own right after it,
  folds it to begin with; `<!-- markmap: foldAll -->` folds it and everything
  below it that has children.
- **Folding by a click**: a press on a node's circle (live preview and reading
  view) folds or unfolds the branch and draws the map again. It is view state
  only: it is not written to the note, it is kept per text of the fence by the
  window's renderer (so a split of the same note agrees) and it is gone when
  the text changes. `MarkdownEditor::CodeBlockResult::hotspots` and `activate`
  carry it: any code-block renderer can mark places on its picture that answer
  a click, with the picture they lead to, instead of the caret; a click
  anywhere else on a map puts the caret in the block, as for any diagram.
- **Layout**: a tidy tree, text measured with `QFontMetricsF` in the theme's text
  and monospace fonts: each node's subtree is as tall as its children stacked
  (6 px apart) or the node itself, and a node is centred on its first and last
  child. A map wider than the editor is scaled down to fit it (and its hotspots
  with it); there is no zoom or pan.
- **Colours** are the theme's tokens: branch colours `--color-blue`,
  `--color-orange`, `--color-green`, `--color-red`, `--color-purple`,
  `--color-cyan`, `--color-pink`, `--color-yellow` in that order (as first used
  over the whole tree, so folding does not recolour), text `--text-normal`,
  links and the root `--interactive-accent`, the card `--background-primary`.
  The theme (with its fonts) decides them, so a dark and a light theme each
  have their own SVG.
- **Off the UI thread, and capped**: parse, layout and raster run on a pool
  thread of their own (one thread; a thousand nodes parse and draw in about 40
  ms here); the UI thread reads the theme and queues the job. At most 1000 nodes
  are drawn (`kMarkmapMaxNodes`); a bigger outline is cut there and the map
  says "Showing the first 1000 of N nodes" under itself.
- **Publish, PDF export**: nothing is cached on disk. Both draw the map from the
  fence's text (`diagramCachedSVG` for Markmap is `drawMarkmap(parseMarkmap(...))`
  in the theme the output has: the site's dark theme for publish, the paper's
  light theme for a PDF whatever the window uses), so a note never opened in the
  editor still gets its map, as the source asks for it (not as a click folded it).
  With the plugin off (`publish::Options::mindMaps`, `PdfOptions::mindMaps`) they
  show the plain listing. An encrypted note is not drawn in the editor or in a publish; a PDF of an unlocked one
  draws its maps from the text in memory.
- **Show as mind map**: the command "Mind maps: Show as mind map" (`markmap:show-note`,
  off with the plugin) opens the whole note as a map in a window of its own, as
  markmap.js reads a document: headings, lists and text, less the frontmatter and
  the fenced code blocks (`markmapSourceOfNote`), in a reading-view editor, so the
  circles fold there too. The root is the note's single top heading; with several
  top-level nodes it is the empty one.

Not covered, against markmap.js: zoom and pan, the toolbar, dragging nodes,
clicking a link (the map is a picture), maths, images, tables and code blocks inside
nodes, checkbox boxes (a task's `[x]` is text), the `<!-- markmap: ... -->`
commands other than `fold` and `foldAll`, other frontmatter options (`color`,
`duration`, `zoom`, ...), and the Obsidian plugin's per-note frontmatter
(only the fence's own is read).

Tests: `MarkmapTreeTest.*`, `MarkmapDrawTest.*`, `MarkmapBlockTest.*`,
`MarkmapWindowTest.*` (`tests/test_markmap.cpp`) and
`PublishTest.AMindMapIsDrawnFromTheFencesTextEvenInANoteNeverOpened`; the demo vault's
`Diagrams/Markmap.md`.

## File cards

A file with no richer renderer embeds (`![[x.bin]]`) as a card: icon, name,
size, a click opens it. An `.epub` card shows the book's cover in place of the
icon: the manifest item with `properties="cover-image"` (EPUB 3), else the one
`<meta name="cover">` names (EPUB 2). The cover is read off the UI thread
(`libs/ext/epub/EpubCover.cpp`, a zip walk and an image decode, scaled to at
most 256 px) and kept per path, mtime and size; the card is laid out without it
first and grows from 52 to 96 px when it lands. A book with no cover, or one
that does not decode, stays the plain card. Built with the EPUB module
(`NOTEAPPC_WITH_EPUB`); without it the card is always the plain one.

## Renaming and links

With `links.autoUpdate` on, renaming a note rewrites the links that point at
it (`Index::updateLinksAfterRename`). Renaming a folder does the same for
every link that resolves to a note under it: `[[Folder/A]]` and
`[y](Folder/A.md)` become `Moved/A`, `Moved/A.md`, a bare `[[A]]` stays
`[[A]]` while the name is unique, and a linking note that sat inside the
folder is rewritten at its new path. Each linking note is rewritten from the
text on disk at that moment, not from where the index last saw its links, so a
note saved a moment ago is never spliced at stale offsets. A linking note that
is not valid UTF-8 is left byte for byte as it is and named in the status bar; the other notes are still
rewritten.

Renaming from the file tree (F2, or Rename in its menu) edits the name in place and goes
through the same path as the inline title.

Moving does the same as renaming. Drag a note or folder onto a folder in the
file tree (onto empty space for the vault root), pick "Move to..." in its
menu, or run "Move file to another folder" for the open note: a fuzzy picker
over the vault's visible folders (`/` is the root). A path-style link follows
the note, a bare `[[Name]]` stays while the name is unique, open tabs, the
file icon and the history come along, and unsaved typing is saved first. A
folder cannot go into itself or its own subfolder, onto a name that is taken,
or into `.obsidian`, `.trash` or another hidden folder. Changing only the
letter case of a name works on case-insensitive filesystems (exFAT, NTFS,
SMB) too.

A moved note's own Markdown links and embeds are paths from its folder, so
they are rewritten to reach the same files from the new one: `[a](../a.md)` and
`![](img/p.png)` moved a level down become `[a](../../a.md)` and
`![](../img/p.png)`. Percent-encoding and `<angle bracket>` forms are kept as
written (`[x](<a b.md>)` is a link to `a b.md`, a backlink like any other, and a
rename rewrites what is inside the brackets). Wikilinks (found by name), URLs, `#anchor` links and bare `[x](a.md)`
names are left alone, and a folder moved whole keeps the links between its own
files byte for byte. A Markdown link to a moved note from elsewhere stays a
relative path (`../A/n.md` becomes `../A/B/n.md`).

A rename or move first saves every note with unsaved typing, whatever `files.autosave`
says, so the rewrite works on what is on screen and the next autosave cannot
put the old link back. The open editor then picks up the rewritten text from
disk. A link typed in the last two seconds is saved but may not be indexed
yet, so that one link is not rewritten.
