---
tags: [demo, embeds, pdf]
---

# PDF embeds

Target file: `assets/graphviz-dot-manual.pdf`, a real 9-page manual page
(source and licence in `docs/DEMO-ASSETS.md`), chosen specifically so every page
argument below has real, different pages to show.
Page lists, ranges, `all`, `#height=` and the slide/column/grid layouts are
all shown below; any that do not render fall back to page one or a plain link.

## No arguments: the whole document

Obsidian scrolls a plain PDF embed page by page; here it can also be
presented as slides instead of a scrolling column - see the layout
arguments further down.

![[graphviz-dot-manual.pdf]]

## One page

`#page=3`, Obsidian's own form - should show page 3 alone:

![[graphviz-dot-manual.pdf#page=3]]

## Several pages

`#page=2,4` - pages two and four, nothing in between:

![[graphviz-dot-manual.pdf#page=2,4]]

## A page range

`#page=3-7` - every page from three to seven:

![[graphviz-dot-manual.pdf#page=3-7]]

## Mixed list and range

`#page=1,3-4` - page one, then pages three and four:

![[graphviz-dot-manual.pdf#page=1,3-4]]

## Every page, explicitly

`#page=all`:

![[graphviz-dot-manual.pdf#page=all]]

## A fixed height

`#height=400` - the embed's frame capped at 400px tall regardless of the
page's own aspect ratio:

![[graphviz-dot-manual.pdf#height=400]]

## Layouts

Column layout (Obsidian's default, made explicit): `&layout=column`

![[graphviz-dot-manual.pdf#page=1-3&layout=column]]

Slide layout - one page at a time with previous/next and a page counter,
instead of a scrolling column: `&layout=slides`

![[graphviz-dot-manual.pdf#page=1-4&layout=slides]]

Grid layout, three columns: `&layout=grid&columns=3`

![[graphviz-dot-manual.pdf#page=all&layout=grid&columns=3]]

## A bad argument

`#page=99` - past the document's last page - should not crash, and should
show either nothing, an error, or clamp to the last real page:

![[graphviz-dot-manual.pdf#page=99]]

`#page=banana` - not a number at all:

![[graphviz-dot-manual.pdf#page=banana]]
