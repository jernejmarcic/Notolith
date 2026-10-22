---
tags: [demo, canvas]
---

# Canvas

What to look for: `Board.canvas`, in this folder, opens as a board of cards,
groups and connectors (a valid JSON Canvas 1.0 file) when Settings > Core
plugins > Canvas is on. "Canvas: Create new canvas" makes an empty one.

## Try

1. Open `Board.canvas`. The wheel pans, Shift+wheel pans sideways, Ctrl+wheel
   zooms about the pointer. Middle-drag or Space+drag also pans.
2. Click a card to select it, Shift-click to add another, drag on empty canvas
   for a selection box, Ctrl+A selects all.
3. Drag a card to move it (a group carries the cards inside it). Drag a corner
   handle of the one selected card to resize it.
4. Hover a card and drag from one of its four side handles onto another card to
   connect them. Del removes the selection and its edges.
5. Double-click a text card to edit it in place (Escape commits). Double-click
   empty canvas for a new text card. Double-click a file card to open the note.
6. Click a card whose text runs past its bottom, then use the wheel over it: it
   scrolls the card, not the board.

## Snapping

Two switches under Settings > Core plugins, below the plugin list:

- "Canvas: snap to objects" (on): while you move or resize a card its edges and
  centre jump to another card's within 6 pixels, with a guide line.
- "Canvas: snap to grid" (off): what is not snapped to a card rounds to a
  32-pixel grid.

A canvas can sit in a split or a pop-out window like a note.
