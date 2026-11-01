---
tags: [demo, embeds]
---

# Image embeds

What to look for: every form below should show the actual picture, sized as
its arguments say; the wikilink and Markdown forms of a plain embed should
look identical. The image is a small gradient made for this vault (see `docs/DEMO-ASSETS.md`
at the project root).

## Wikilink embed, natural size

![[gradient.png]]

## Wikilink embed, width only

`|200` should cap the width at 200px, height scaled proportionally:

![[gradient.png|200]]

## Wikilink embed, width and height

`|200x100` should force an exact box, not necessarily proportional:

![[gradient.png|200x100]]

## Markdown-style image embed

The same file, Markdown syntax instead of a wikilink - should render
identically to the natural-size wikilink embed above:

![gradient](assets/gradient.png)

## Several embeds in one paragraph, and on one line

Two embeds side by side on one line, one by name and one by path:
![[gradient.png|80]]
![[assets/gradient.png|80]] - both should render inline without a
paragraph break between them.

## An embed inside running text

Here is the image ![[gradient.png|32]] sitting inline with a sentence that
continues on either side of it, the way an emoji or an inline icon would.
