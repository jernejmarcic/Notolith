# Glyph studio (notolith-hwfont)

Write each letter a few times with the pen; every capture is one `.ink` file under
`<vault>/Handwriting/<font>/<hex>/<n>.ink` (hex = the code point, `0061` for "a"). The module is its
own install unit (`notolith-hwfont`): it links `libnotolith-inkcore.so` from the package `notolith-ink`
and never loads the `notolith-ink` module itself.

- Open it with the command "Open glyph studio" (feature `handwriting-font`).
- The studio is a tab that belongs to no file. It counts as modified while the cell holds strokes; the
  host saves it when the tab closes or Notolith quits (the samples are never lost).
- Keys, all with buttons beside the cell (the pen's tip is the only thing PADD has): Enter saves the
  sample and moves to the next glyph that wants more, Backspace discards the cell, Ctrl+Z undoes a
  stroke, Ctrl+Shift+Z or Ctrl+Y redoes it, Page Down skips to the next glyph. The pen's barrel
  button (a click, not a drag) saves too, when the pen has one. Escape does nothing.
- "New font..." asks for a name (refused when it could leave `Handwriting/`), switches the studio to it and stores it as `hwfont.fontName`.
- The cell draws with the mouse as well as the pen, whatever `ink.drawWithMouse` says; the keys work as soon as the tab is shown.
- Click a row of the list on the right to write that glyph. "a 1/3" is one of three samples.
- Settings: `hwfont.fontName` (a choice among the folders already in `Handwriting/`, default
  "My handwriting"; a new font is a new folder), `hwfont.samplesPerLetter` (3),
  `hwfont.samplesPerPunctuation` (1). A font name that could leave `Handwriting/` is refused and the
  studio shows why.

Moves into `docs/INK.md` (`## Glyph studio`) when Track S1 lands.
