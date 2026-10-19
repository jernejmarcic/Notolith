---
tags: [demo, editor, vim]
---

# Vim key bindings

What to look for: with Settings > Editor > "Vim key bindings" **on** (it is off
by default; off, the editor is unchanged and pays one bool test per key), the
editor is modal. `Esc` (or `Ctrl+[`) is normal mode with a block caret, `i`
types, `v` selects. The mode, a half-typed command such as `2d` and the `:` or
`/` line show in the status bar. This note is a practice sheet: each section
has text to act on and the keys to try, with the result to expect. Use Ctrl+Z
(or `u` in normal mode) between exercises, or just carry on.

In normal and visual mode the caret's block always shows its source, also
while something is selected.

## Motions

Put the caret at the start of the line below in normal mode (`Esc`, then `0`).
`w` moves a word, `3w` three words, `$` to the end, `^` to the first non-blank,
`e` to a word's end, `b` back a word, `f,` to the next comma, `;` repeats it.

the quick brown fox jumps over the lazy dog, then rests, then sleeps

`gg` goes to the top of the note, `G` to the end, `12G` to line 12, `{` and
`}` by paragraph, `%` to the matching bracket in `(a (b) c)` from the first
bracket, and `*` searches for the word under the caret (`n` and `N` go on).

## Operators and text objects

`dw` deletes a word, `d2w` two, `dd` a line, `cc` changes a line, `yy` yanks it
and `p` puts it below. Text objects work after an operator: `diw` deletes the
word under the caret, `ci"` changes inside quotes, `dap` deletes a paragraph,
`dit` deletes inside an HTML tag, `ci(` changes inside brackets.

Try `ci"` with the caret anywhere inside the quotes: she said "change this
text" and left. Try `dit` on <b>this bold part</b> of the line. Try `ci(`
on this (parenthesised words).

## Counts and repeat

`3x` deletes three characters, `5G` goes to line 5, `3J` joins three lines,
`.` repeats the last change. Delete the next word with `dw`, then press `.`
twice: one two three four five six.

## Visual mode

`v` selects characters, `V` whole lines and `Ctrl+v` a block of columns; `o`
swaps the ends of the selection; `d`, `y`, `c`, `>` and `<` act on it. In block
mode `I` and `A` insert on the top line and copy what you typed to every line
when `Esc` ends it. Try `Ctrl+v`, `jj`, `A`, ` done`, `Esc` on these three
lines:

first line
second line
third line

## Registers and marks

`"ayy` yanks the line into register `a`, `"ap` puts it. `"+y` yanks to the
system clipboard, `"_dd` deletes without touching any register. `ma` sets mark
`a`, `'a` jumps back to its line, `d'a` deletes from here to it.

## Macros

`qa` starts recording into `a`, `q` stops, `@a` plays it, `3@a` plays it three
times, and the status bar shows `recording @a`. Record `0i- ` then Esc `j` on
the first of these lines and replay it to make a bulleted list:

apples
pears
plums
figs

## The ex line

`:w` saves, `:q` closes the tab (refused with unsaved changes), `:wq` does
both, `:12` goes to line 12, `:noh` clears search highlights, and `:s` replaces:
`:%s/colour/color/g` on every line, `:3,5s/a/b/` on a range, `:'<,'>s/x/y/` for
a visual selection (typing `:` in visual mode fills the range in). The pattern
is a regular expression, and `:s` is case-sensitive unless it ends in `i`.
Try it on this sentence: the colour of the colour wheel.

## Search

`/pattern` searches forward and `?pattern` back, `n` and `N` step through the
matches, shown with the editor's find highlight. The pattern is a regular
expression that ignores case unless it has a capital. Search for `wheel`.

## What does not work

`:set`, a count on `.`, `p` over a visual selection, `gu`, `gU`, `g~`, `U` as
line undo, and `:s` flags other than `g` and `i` are not covered. `Ctrl+C`,
`Ctrl+S` and the other Ctrl chords that Vim mode does not own still reach the
editor in normal mode.
