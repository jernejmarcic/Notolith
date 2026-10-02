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
(`align=` on the first row's cells read), and inline `b` `strong` `i` `em` `u`
`s` `del` `strike` `mark` `sub` `sup` `kbd` `code` `a` (`href` is the link)
`span` (a `style=` holding only `color:`). Other attributes (`class`, `id`)
are kept and ignored.

**Left to litehtml** (drawn as an image, clicked into it shows its source, as
before): a block with any other tag (`img`, `svg`, `iframe`, `video`,
`details`/`summary`, unknown tags), `style=` on anything but a colour span, a
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

Not done: an HTML heading is not in the outline (headings come from the
Markdown parse), entities in plain Markdown text are not decoded, a `style=` on
a cell sends the whole table to litehtml.

Tests: `tests/test_html_edit.cpp` (`ctest -R html_edit`).

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
