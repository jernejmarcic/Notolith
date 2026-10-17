# The editor

`libs/editor/MarkdownEditor` is the one editing widget: live preview, source
and reading are three modes of it. This page covers the optional parts of how
it takes input: [HTML edited in place](#html-edited-in-place),
[the `</>` edit button](#the--edit-button-on-rendered-blocks) and its
[callout menu](#the-callout-menu), [embedded notes](#embedded-notes),
[the status bar counts](#status-bar-counts) and
[Vim key bindings](#vim-key-bindings).

## Scripts never run

Jernej, 2026-10-16: "javascript should never be ran"; only HTML and CSS
formatting is followed, even where a page relied on a script. There is no
script engine anywhere in the app: litehtml has none, and `sanitise()`
(libs/render/HtmlRenderer.cpp) strips `<script>` and every `on*=` attribute
before litehtml sees the markup. What a script element says is shown in its
place instead, in `--text-error` and bold: an HTML block draws it as text
(libs/editor/HtmlBlock.cpp layoutHtml), an inline `<script>...</script>` keeps
its tags showing (inlineHTMLpairs). Links open only http, https and mailto,
from notes and PDFs alike, so a `javascript:` URI is never handed on. The
plugin manifest's QuickJS engine kind is a seam with no engine behind it.

## HTML edited in place

Jernej, 2026-10-02: "For HTML I want rendering of it like in obsidian, but
unlike obsidian I want it inline editable just like the markdown", and "if its
a HTML table it should be editable like an actual table element (like in excel
for example, clicking on cells, etc.)".

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

Tests: `tests/test_html_edit.cpp` (`ctest -R html_edit`).

## Lists

Jernej, 2026-10-16, testing the app: "Pressing return after a counter does not
spawn another like it should", "Numbers should continue logically", "I also
want different ones (-,*,=) to render differently", and a hierarchical number
"first number.second number so this would be 1.2.", toggleable, ending in "a ."
or ":" or literally anything.

**Why Enter did not continue a list.** It did, but only for a bare list: the
list-line pattern (`EditingCommands.cpp`, `parseListLine`) did not know a quote
prefix, so `> - x` and `> 1. x` in a quote or callout fell through to a plain
newline. A line now reads `quote + indent + bullet + spacing + [checkbox]`.

**Keys** (all in `EditingCommands.cpp`, one undo step each, byte-exact apart
from the numbers they renumber):

| key | on a list item |
|---|---|
| Enter | a new item with the same marker and indent (`-`, `*`, `+`, `N.`, `N)`, a task becomes `- [ ] `, inside `> ` too). Ordered counts on and renumbers the items after it. Mid-item it splits the item. An empty item leaves in steps: out of a quote level first (`> - ` becomes `- `), then out one indent level, then the marker goes. |
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

Jernej, 2026-10-16: "when hovering over rendered elements the code icon should
appear to edit the code". Behaviour is Obsidian's "Edit this block"; the code
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

Titles: a whole-note embed carries the note's name over it, a heading embed
"Note > Heading", and a `#^id` block embed none (Obsidian shows a block embed
bare). Of links, `[[#^id]]` shows "^id" (only a leading `#` is hidden, as
in Obsidian's live preview) and `[[Note#^id]]` shows as written; a heading
link shows "Note > Heading".

## Autosave

With `files.autosave` on (the default) every modified note is saved two
seconds after typing stops, and leaving a note, closing a tab or quitting saves
it too; nothing asks. A save that fails (disk full, read-only folder) says so in
the status bar and is tried again at the next pause, with no further edit
needed. If leaving or quitting then still cannot save, the usual
Save / Discard / Cancel box appears with the error in its text, so a window is
never refused a close without a word. With the setting off, the box is always
the way out and nothing is saved on a timer.

## Status bar counts

Words and characters in the bar skip the frontmatter (the same text the word
count already skipped). With a selection the bar shows the selection's
instead, "N words, M characters", as Obsidian does. The note's counts still
come with each parse; the selection's are recounted 200 ms after the caret
last moved (`MainWindow::m_selectionCountTimer`), never on the keystroke or drag
path, and cost one pass over the selected text.

## Fenced code and the caret

The caret anywhere from a fence's opening line to its closing line, not only on
a fence line, shows both fence lines and the language tag (Obsidian treats the
block as one unit), and moving out hides them again. md4qt's code block
covers the listing alone, so `fenceAroundCaret()` marks the blocks either side
as active and `revealedSyntax` reveals their fence markers. A listing inside a
callout wraps in live preview, as Obsidian's does (a language-less listing
there scrolls on a single unbroken token; here it is broken, which no
reader has asked about).

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
its body), no properties table, tables, embeds, maths, queries or bullets. A
renderer that forgets to ask `plainSource()` shows up as a panel in Source mode.

## Vim key bindings

Settings > Editor > "Vim key bindings" (`editor.vimMode`, off by default).
Off, `keyPressEvent` pays one bool test and nothing else runs. There is no
CMake option, because there is no dependency to drop: the whole thing is
`libs/editor/VimMode.cpp` and a small state struct in `MarkdownEditor.h`.

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

## Mermaid diagrams

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
  runs `mmdr -i - -c config.json` on a one-thread pool (source on stdin, SVG on
  stdout; `-o -` is not used, 0.3.1 writes a file named `-` for it) and
  rasterises the SVG with `QSvgRenderer` at the column width and the display's
  pixel ratio. The tool gets 5 s, then is killed.
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
  a dark one is a dark card on a light page). Publish inlines it as
  `<div class="mermaid-diagram">` with its ids prefixed per diagram; PDF export
  draws it as an image. A fence never drawn in the editor is its source, as a
  code block.
- **Not drawn**: encrypted notes (the source would go into the cache), slides.

`tests/test_mermaid.cpp` runs all of this against a fake `mmdr` script, plus
one test against the installed tool (skipped when absent, a failure under
`NOTEAPPC_REQUIRE_TOOLS=1`, which the lane check sets); the publish test is in
`tests/test_publish.cpp`.

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
folder is rewritten at its new path.

Renaming from the file tree (F2, or Rename in its menu) edits the name in place and goes
through the same path as the inline title.

A rename first saves every note with unsaved typing, whatever `files.autosave`
says, so the rewrite works on what is on screen and the next autosave cannot
put the old link back. The open editor then picks up the rewritten text from
disk. A link typed in the last two seconds is saved but may not be indexed
yet, so that one link is not rewritten.
