---
tags: [demo, editor, pdf]
---

# PDF viewer

What to look for: with Settings > Core plugins > PDF viewer on, double-clicking a
PDF in the tree opens it in a tab you can read and mark up. Off, it opens as it
always did. Compare with [[PDF Embeds]], which draws pages inside a note.

**Marks are saved into the PDF file itself.** Do not mark up the shared
`Embeds/assets/graphviz-dot-manual.pdf` that other notes embed: copy it first
("Make a copy" in the tree's context menu) and open the copy.

## Try

1. Open the copy. The toolbar has a page field, zoom in and out, Fit width, Fit
   page, a Contents toggle for the outline, and the annotation tools.
2. Pick Highlight, Underline or Strike and drag over some text. Pick Ink and
   draw freehand. Pick Note and click to place a comment. Pick Box and drag a
   rectangle. Colour has five choices; with a mark selected it recolours it.
3. Select a mark (the Select tool) and press Delete to remove it, or edit its
   comment. Undo and Redo work on every mark.
4. Ctrl+F searches the text.
5. Save. Reopen the file: the marks are still there, in any other PDF reader too.
6. Run "PDF: Extract annotations to a note". A note beside the PDF lists each
   mark with its page and text, to link, search and edit.

A link such as `[[graphviz-dot-manual.pdf#page=3]]` opens the PDF at page 3.
Delete your copy and its extracted note when you are done.
