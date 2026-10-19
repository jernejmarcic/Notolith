# Canvas

A `.canvas` file opens as a board of cards and connectors (JSON Canvas 1.0).
`libs/ext/canvas`, behind `NOTEAPPC_WITH_CANVAS`; the Core plugins switch
"Canvas" turns it off.

## Controls

- **Pan and zoom** - the wheel pans, Shift+wheel pans sideways, Ctrl+wheel (or
  Space held and the wheel) zooms about the pointer; middle-drag or
  Space+drag pans.
- **Select** - click a card; Shift-click adds or removes; drag on empty
  canvas for a selection box (Shift adds); Ctrl+A selects all.
- **Move and resize** - drag a card (a selection moves as one block; a group
  carries the cards inside it); drag a handle of the one selected card to
  resize it. Each drag is one undo step.
- **Connect** - hover a card and drag from one of its four side handles to
  another card. Del removes the selection and every edge touching it.
- **Edit** - double-click a text card for the in-place editor (Escape or a
  click outside commits it); double-click empty canvas for a new text card;
  double-click or Ctrl-click a file card to open the note.

### Snapping

Two switches, in Settings, Core plugins, under the feature list, and applied
to open canvases as they are flipped:

- **Canvas: snap to objects** (`canvas.snapToObjects`, on by default). While a
  card is moved or resized, its edges and its centre jump onto another card's
  left / centre / right (or top / centre / bottom) when they come within 6
  screen pixels - so the reach is the same at any zoom - and a 1 px guide line
  in the accent colour (`--interactive-accent`) runs along the match, out to
  the full extent of every card on that line. A multi-selection snaps as one
  block: its outer edges and centre, not each card's own. A resize snaps only
  the edge being dragged (a corner, its two edges). Groups are targets too;
  the cards that move with the drag are not.
- **Canvas: snap to grid** (`canvas.snapToGrid`, off by default). What is not
  snapped to a card rounds to the 32-pixel dot grid: a move rounds the
  selection's top-left corner, a resize the edge being dragged. A card beats
  the grid, axis by axis.

Off, the lists snapping searches are never built. On, they are built once, at
the first real movement of a drag (not on a click), and each mouse move is a
handful of binary searches.

### Scrolling a card

Click a text card or a `.md` file card (a `#heading` or `#^block` section
too) to select it; if its content runs past its bottom, the wheel over it then
scrolls the card's content, and a thin thumb shows where you are. It stops at
both ends (no scroll chaining into a pan). Everything else keeps the wheel:

- over a card that is not selected, or a selected one whose content fits, the
  wheel pans the canvas;
- Ctrl+wheel always zooms;
- link cards, images, other file types and groups have nothing to scroll;
- while a text card is being edited, its editor takes the wheel.

The scroll position is not kept: deselecting the card, editing it, or
changing its content or size puts it back at the top. The picture is a hidden
Reading-mode editor holding the whole note, grabbed at the scrolled position
each frame - no parse or relayout per wheel tick (the guard in
docs/PERFORMANCE.md). The cached picture at scroll 0 stays valid for when the
card is deselected.

## Where the settings live

The two snap switches are ordinary settings (`canvas.snapToObjects`,
`canvas.snapToGrid`), stored like the rest in the app's settings file. Obsidian
keeps its own two toggles somewhere this app does not read - it was not found
where - so a vault's Obsidian state is not imported.

## Chosen here, not confirmed against Obsidian

Written from Obsidian's public documentation and memory, not a side-by-side
run: the 6 px reach, the defaults (objects on, grid off), that groups are snap
targets, that the grid rounds the top-left corner, that a selected card whose
content fits lets the wheel pan, and that the scroll position is not kept
across a deselect. Alt does not suspend snapping.

## Checking it without a display

`ctest --test-dir <build> -R canvas` runs the canvas tests offscreen
(`QT_QPA_PLATFORM=offscreen`): pixel probes read the guide lines, the wheel
tests send `QWheelEvent`s to the view. What it cannot show: touchpad pixel
delta phases and kinetic scrolling, how the guides look at a high
`devicePixelRatio`, and how the 6 px reach and 32 px grid feel under a hand.
