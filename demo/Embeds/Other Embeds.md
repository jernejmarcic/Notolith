---
tags: [demo, embeds]
---

# Canvas, base, and unknown-file embeds

## A canvas embed

![[Board.canvas]]

See `Canvas/Board.canvas` for the file itself, opened directly as a tab.

## A base embed

![[Library.base]]

See `Library/Library.base` for the file itself and [[Library]] for the
collection of notes it queries.

## An unknown binary file

`color-profile.icc` is a real ICC colour profile (source and licence in
`demo-assets.md`) with an extension noteAppC does not recognise as an
image, audio, video or PDF - it should fall back to a generic file card
(name, size, an icon, a way to open it externally) rather than trying to
read it as text:

![[color-profile.icc]]
