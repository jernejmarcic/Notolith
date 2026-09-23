---
tags: [demo, embeds]
---

# Image embeds

What to look for: every form below should show the actual picture, sized as
its arguments say; the wikilink and Markdown forms of a plain embed should
look identical. Source and licence for both images are in `demo-assets.md`
at the project root.

## Wikilink embed, natural size

![[nvim-logo.png]]

## Wikilink embed, width only

`|200` should cap the width at 200px, height scaled proportionally:

![[nvim-logo.png|200]]

## Wikilink embed, width and height

`|200x100` should force an exact box, not necessarily proportional:

![[nvim-logo.png|200x100]]

## Markdown-style image embed

The same file, Markdown syntax instead of a wikilink - should render
identically to the natural-size wikilink embed above:

![nvim logo](assets/nvim-logo.png)

## Several embeds in one paragraph, and on one line

Two different images side by side on one line: ![[nvim-logo.png|80]]
![[assets/kdoctools-banner.jpg|80]] - both should render inline without a
paragraph break between them.

## An embed inside running text

Here is the logo ![[nvim-logo.png|32]] sitting inline with a sentence that
continues on either side of it, the way an emoji or an inline icon would.
