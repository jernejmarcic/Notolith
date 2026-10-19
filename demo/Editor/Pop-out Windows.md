---
tags: [demo, editor, windows]
---

# Pop-out windows

What to look for: a tab can live in a window of its own, a sidebar can leave
the main window, and a tab dragged out of its tab strip can be tiled into a
split. There is still one vault, one index and one set of side panes: the right
sidebar and the status bar follow whichever tab has the focus, in whichever
window it is. Settings > Core plugins > "Pop-out windows" is Notolith's own
switch (Obsidian has none): off, the commands and menu items stop, every tab in
a pop-out returns to the main window, every detached sidebar goes back, and no
window object or drop target exists.

## Open a tab in a window

Run "Open current tab in new window" (also on the tab's right-click menu,
beside Split right and Split down): this note shows again in a new window, and
the main window keeps its own tab.

## Move a tab to a window

"Move current tab to new window" takes the tab itself, with its history, caret
and view (a canvas or a PDF comes along). A pop-out has tabs and splits like
the main window but no sidebars, header or status bar, and is titled by its
current tab. Every hotkey works in it. If the main window had only this tab,
it is left with an empty "New tab": the main window never has none.

## Closing

Closing a pop-out closes its tabs like any other close: the note is saved first
when autosave is on, and Ctrl+Shift+T brings the last one back, in the main
window. A pop-out whose last tab goes closes with it. Closing the main window
closes every pop-out with it, after saving the layout: the windows, their
splits, tabs and sizes (not positions, the compositor decides) come back at the
next start, and Obsidian's own `floating` entries in the workspace are read the
same way.

## Detach a sidebar

"Detach left sidebar" and "Detach right sidebar" (Notolith's own commands;
Obsidian only drags) put the whole sidebar in a window. The same entry is
"Detach sidebar" on the right-click menu of the left header or of the right
tab row. Right-click one tab of the right sidebar for "Detach pane", which takes
that pane alone and hides its tab. The detached sidebar keeps working: the file
tree, backlinks, outline and the rest follow the active note. Closing its window
puts it back where it was. While a sidebar is detached "Toggle left sidebar" and
"Toggle right sidebar" leave it alone, and clicking the status bar's backlink
count raises its window instead of switching panes. Detached sidebars are not
remembered across restarts (pop-outs holding tabs are).

## Drag to tile

Drag this note's tab out of its tab strip (a drag inside the strip only
reorders). While the pointer is over a split, a translucent zone shows where
the tab will land:

- the outer quarter along an edge: a new split there, left or right side by
  side, top or bottom stacked, holding the tab;
- the rest of the body: the tab joins that split;
- a tab strip: the tab goes in at the tab under the pointer.

This works between windows too. Let go outside every Notolith window and the tab
opens in a window of its own, as "Move current tab to new window" does; let go
over a sidebar or the header and nothing happens. Dropping a split's only tab
on its own edge does nothing, there being nothing to split from. A drop only
moves a tab: none is made or lost, and its history, caret and view stay.

## Limits

- Escape during the drag, with the pointer outside every window, also opens the
  tab in a window of its own (Qt reports the two the same).
- Tabs in the sidebars cannot be dragged; use "Detach pane".
- The settings page, dialogs and the graph open in the main window.
