# The editor

`libs/editor/MarkdownEditor` is the one editing widget: live preview, source
and reading are three modes of it. This page covers the optional parts of how
it takes input: [HTML edited in place](#html-edited-in-place),
[the `</>` edit button](#the--edit-button-on-rendered-blocks) and
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
`font-weight` (a value with a `:` or `url(` is not); only the alignment is
drawn, the rest stays in the file and is ignored.

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
| text objects | after an operator, or in visual mode: `iw aw iW aW`, `is as`, `ip ap` (lines), `i" a" i' a'` and ``i` a` `` (on the line), and brackets `i( a( ib ab`, `i[ a[`, `i{ a{ iB aB`, `i< a<` (the closing character names them too). A count means that many words, sentences or paragraphs, or that many bracket levels out. `i{` on a block whose braces sit on their own lines takes the lines between |
| `%` | jumps to the matching `()`, `[]` or `{}`: from the first bracket at or after the caret on the line; works across lines; usable after an operator (`d%`) |
| `* #` | search for the whole word under the caret, forward or back; `n` and `N` go on from there |
| indent | `>` and `<` move lines by the editor's tab width (Settings > Editor), `<` also takes one leading tab; empty lines stay empty |
| registers | `"a`-`"z` before a yank, delete, change or put (`"ayy`, `"ap`); `"A`-`"Z` append; `"0` is the last yank, `""` the unnamed one; `"_` is the black hole; `"+` and `"*` are the system clipboard (a put from it is lines if the text ends in a newline) |
| marks | `ma`-`mz` set; `'a` goes to the line and the backtick form to the exact place, both usable after an operator (`d'a`); `''` returns to where the last jump began (`G gg % n N * # { } :N` and mark jumps are jumps); marks follow edits; `'<` and `'>` are the last visual selection |
| macros | `qa` ... `q` records into `a` (`qA` appends), `@a` plays it, `@@` plays the last one again, `3@a` three times; the status bar shows `recording @a`. A macro is the keys as typed, so insert-mode typing replays too. One that runs itself stops after 8 levels |
| block visual | `Ctrl+v`, then motions (`$` runs every line to its end); `d x c y` act on the columns, `I` and `A` insert on the top line and copy the typed text to every line when `Esc` ends it (`I` skips lines too short to reach the block, `A` pads them), `>` `<` shift the lines; a yanked block is put back as a block |
| put and edit | `p P` (character, line or block, as yanked), `r<char>`, `~`, `J` |
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

`:set`, a count on `.`, `p` over a visual selection, `J`, `r` and `~` in visual
mode, numbered registers `"1`-`"9`, a count on `%` (go to N percent), a text
object that extends an existing visual selection (it replaces it), `it`/`at`
(tags), `:s` flags other than `g` and `i`, and a macro stopping at its first
failed motion (it runs on). Block columns count characters, not drawn width, so
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
