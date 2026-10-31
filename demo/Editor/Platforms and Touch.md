---
tags: [demo, editor, platforms]
---

# Platforms and touch

What to look for: how the app starts, and what changes on a touchscreen or a
narrow window. Nothing here needs a setting.

## One window per vault

Launching `notolith some-note.md` while Notolith already has that note's vault
open does not open a second window: the running one opens the file and comes to
the front. A note inside the open vault replaces the current tab; a note
outside it opens beside, the vault staying open. A different vault gets a
window of its own, so two vaults are two windows. Choosing a vault that is
already open in another window brings that window forward instead. `notolith
note.md` with nothing running opens that one file with no vault.

## Touch

- A finger scrolls with momentum, and a long press selects a word.
- In a table the first tap shows the grips and add bars, and a long press in a
  cell lets the finger drag a rectangle of cells ([[Table Editing]]).
- On a narrow window the sidebars fold away. Resize this window to a phone width
  to see it.

## Other platforms

The code has no CPU-specific instructions, so an ARM build has nothing in the
way. An Android build is in progress; there is no mobile app yet. Painting is
raster by default, with OpenGL as a setting (Settings > Editor > "GPU
acceleration"). The details and the measured numbers are in `docs/PLATFORMS.md`
and `docs/PERFORMANCE.md`.
