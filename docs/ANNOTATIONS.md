# Image annotation

Settings, Core plugins, "Image annotation" (off by default; build switch
`NOTEAPPC_WITH_IMAGEVIEW`, code in `libs/ext/imageview`). On, an image opens in
a tab you can mark up; off, it opens as the plain picture, and the module is
never constructed.

## Tools

Select (click a mark; Delete removes it; double-click edits its text), Highlight
(drag a box), Draw (freehand), Note (click, type, Ctrl+Enter or click away),
Colour (the PDF viewer's five; with a mark selected it recolours it), Undo and
Redo, zoom (Ctrl+wheel, - / + , Fit, 100%). Hover a mark to see its text. Every
coordinate is stored as a fraction of the image, so marks hold at any zoom.

## Where the marks live

The image file is never opened for writing. The marks go to a note beside it,
`<image file name> annotations.md` (`photo.png annotations.md`; the extension is
kept so `photo.png` and `photo.pdf` do not share one), the way the EPUB reader
keeps its highlights. The note is the source of truth and is rewritten after
every edit through `Vault::write`, so its line endings and BOM survive. It is
created by the first mark, not by opening the image.

Between `<!-- imageview:annotations:begin ... -->` and `<!-- imageview:annotations:end -->`
each mark is a readable line (`> Note at 50%, 75%`), a hidden marker that
carries the geometry (`%% image-annotation kind=note color=#ffeb3b at=0.5,0.75 %%`)
and the text under it. Write anywhere outside the section and it stays; edit a
mark's text inside it and the next load picks it up. A marker that does not
parse is skipped, not fatal. Lines of a text that would end an entry (`---`) or
look like a marker are escaped with a backslash and restored on load.

## Extract annotations to a note

The "To note" toolbar button and the command "Image: Extract annotations to a
note" write that section (the image embed, then each mark with its text) to the
sidecar, so the note can be linked, searched and edited; it also recreates a
sidecar that was deleted. `ImageView::extractAnnotations(path)` writes the same
section to another note.

Tests: `tests/test_imageview.cpp` (model, tools, undo, image bytes untouched,
reload from the sidecar), `WindowTest.AnImageOpensAnnotatableOnlyWithImageAnnotationOn`.
