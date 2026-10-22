---
tags: [demo, editor, images]
---

# Image annotation

What to look for: Settings > Core plugins > Image annotation (off by default, so
turn it on). An image then opens in a tab you can mark up. Off, it opens as the
plain picture. **The image file is never changed.**

## Try

1. Open `Embeds/assets/nvim-logo.png` from the tree (see [[Image Embeds]] for the
   same picture inside a note).
2. Tools: Select, Highlight (drag a box), Draw (freehand), Note (click, type,
   Ctrl+Enter or click away). Colour has five choices. Undo and Redo work.
   Zoom with Ctrl+wheel, the - and + buttons, Fit or 100%.
3. Hover a mark to see its text. Select one and press Delete to remove it, or
   double-click to edit its text.
4. The first mark creates a note beside the image, `nvim-logo.png annotations.md`,
   with one readable line per mark and a hidden marker holding its place. It is
   the only place the marks live, so edit or delete it freely. Delete it when
   you are done.
5. "Image: Extract annotations to a note" (or the "To note" button) rewrites that
   note with the image embedded and each mark's text under it.
