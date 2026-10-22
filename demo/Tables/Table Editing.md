---
tags: [demo, tables]
---

# Table editing

What to look for: a pipe table in live preview is a grid you edit directly.
Settings > Editor > "Table handles" (on) gives it grips and add bars,
"Align tables" keeps the source columns lined up. Use the table below.

| Fruit | Colour | Count |
| :--- | :---: | ---: |
| Apple | red | 3 |
| Pear | green | 12 |
| Plum | purple | 7 |

## Try

1. Hover the table. A small grip appears on the left of the row and above the
   column under the pointer. Drag a row grip up or down, a column grip sideways,
   to reorder. The header row stays first.
2. Click the thin "+" strip under the table or at its right edge to add a row or
   a column. Drag it to add several.
3. Drag from one cell into another, or hold Shift and use the arrows, to select
   a rectangle of cells. Ctrl+C copies them as a Markdown table (and as tab
   separated text for a spreadsheet), Ctrl+X cuts, Delete empties them, Ctrl+V
   pastes a table or tab separated text, growing the table if needed.
4. Tab and Shift+Tab move between cells; Enter goes down a row, and adds one
   below the last.
5. Run "Insert table" from the palette for a new 2x2 table.
6. In the palette, the "Table:" commands insert, delete, move and duplicate a
   row or a column, align a column left, centre or right, and sort a column
   ascending or descending. "Format table" and "Format all tables" realign the
   source.

An HTML `<table>` that is a simple grid gets the same handles, see
[[HTML Blocks]]. On a touchscreen the first tap in a table shows the grips, and a
long press in a cell arms a cell selection. See also [[Tables]].
