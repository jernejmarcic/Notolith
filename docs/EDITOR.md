# The editor

`libs/editor/MarkdownEditor` is the one editing widget: live preview, source
and reading are three modes of it. This page covers the optional parts of how
it takes input.

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
