---
tags: [demo, editor, folding]
---

# Modes and folding

What to look for: one note, three ways to see it, and headings and list items
that fold.

## The three modes

- Live preview: syntax is drawn as formatting and shows its source under the
  caret.
- Reading view: Ctrl+E (`markdown:toggle-preview`) toggles it. Nothing is
  editable; links and folds still work.
- Source mode: "Toggle Live Preview/Source mode". Syntax in colours, line
  numbers, no rendered blocks at all. Settings > Editor > "Default view" picks
  which one a note opens in (this vault's `app.json` says source).

The pencil, book or code icon in the status bar shows the mode and has a menu
for all three.

## Rendered blocks

A rendered block (table, callout, embed, diagram, HTML) shows a small `</>`
button at its corner when hovered. It puts the caret in the block's source.
Settings > Editor > "Edit button on rendered blocks" switches it off. The command
"Edit block source" does the same from the palette.

## Folding

An arrow in the margin by a heading folds its section; by a list item it folds
the children. A folded line ends in a small "..." box; click either to open.

1. Fold this heading, then fold it again from the keyboard with "Toggle fold on
   the current line".
2. Run "Fold all headings and lists", then "Unfold all headings and lists".
3. "Fold more" and "Fold less" move one level at a time. "Toggle Properties
   fold" folds the properties block at the top of the note.
4. Close the note and open it again: the folds are remembered for each note.

Settings > Editor has "Fold heading" and "Fold indent" to turn either kind off.
Open [[Headings and Outline]] and [[Long Note]] to practise on.

### A foldable list

- Parent item
  - Child one
  - Child two
    - Grandchild
- Another parent
  - Its child

## Readable line length and line numbers

Settings > Editor > "Readable line length" keeps text to a column; "Line width"
sets how wide; "Line numbers" shows them in source mode.
