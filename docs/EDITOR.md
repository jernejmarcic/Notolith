# The editor

`libs/editor/MarkdownEditor` is the one editing widget: live preview, source
and reading are three modes of it. This page covers the optional parts of how
it takes input: [HTML edited in place](#html-edited-in-place) and
[Vim key bindings](#vim-key-bindings).

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

What it does with the bytes - the file is the truth:

- Typing `&`, `<` or `>` into drawn text writes `&amp;`, `&lt;`, `&gt;`
  (typing inside a tag the caret has revealed edits the tag, unescaped).
- An entity (`&amp;`, `&nbsp;`, `&#169;`, every name HTML defines) shows as
  its character; the caret steps over it as one, and Backspace or Delete
  removes all of it.
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
`![[Note#Heading]]` (an embedded section) still finds Markdown headings only.

Entities in Markdown text: a CommonMark reference (`&amp;`, `&copy;`, `&#169;`,
`&#xA9;`, any name md4qt knows) outside code, maths, HTML, links, wikilinks and
frontmatter, and not escaped with a backslash, shows as its character and is
one step for the caret, Backspace, Delete and Vim's `h` `l` `x`, exactly as in
an HTML block (`Parser.cpp`'s `scanEntities`, drawn through DisplayMap's
substitutions). The source is never rewritten, Source mode shows it as
written, and typing next to one writes plain text, not an entity. It does not
open into its source under the caret, as HTML's does not either; delete it and
type it again to change it. A reference with no `;`, an unknown name, a number
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
| `editor.bulletGlyphs` | `-=dash,*=dot,+=diamond` | the glyph per marker, from `dot` `ring` `square` `diamond` `dash` `arrow` `star` `check` `typed` (the character itself). Settings > Editor shows a drop-down per marker with a preview, and Reset restores the default. |

`=` is not a CommonMark list marker (`- [ ]`, `* ` and `+ ` are), so it cannot
be mapped; the three real ones can. The glyphs are painted in
`BulletGlyphs.cpp` with the muted text colour; the dash is a rounded bar wider
than a dot, the dot a filled circle, the diamond a small rhombus.

`tests/test_lists.cpp` covers every key (asserting the whole note, the caret and
undo), the glyph map, the pixels the editor paints per marker, the number labels
and the timing.

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
no match), but `l` and `h` at a line's edge do not fail as they do in vim.
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
