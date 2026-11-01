---
tags: [demo, links]
---

# Links and references

What to look for: a resolved wikilink is styled differently from an
unresolved one (Obsidian dims/underlines unresolved links, often in a
different colour); hovering a link with Ctrl held (live preview) or plainly
(reading view) should open the page-preview popover at the right heading or
block; clicking a link follows it.

## Plain wikilinks

A plain link: [[Target Note]]. The same note by an aliased display name
(the `|` form): [[Target Note|see the target note]]. The note's own
`aliases:` frontmatter also resolves - linking by its alias works too:
[[The Target]].

## Heading and block links

A link to a specific heading: [[Target Note#Details]]. A link to a specific
block, by the `^anchor-block` id set on a paragraph in that note:
[[Target Note#^anchor-block]].

## Markdown-style links

The same target, written as a Markdown link instead of a wikilink:
[Target Note](Target%20Note.md). A link to a note whose filename has a
space, with the space percent-encoded as it must be in a Markdown link:
[Code Blocks](../Code/Code%20Blocks.md) and [Basic Syntax](../Formatting/Basic%20Syntax.md).

## External links

A bare Markdown link: [Obsidian](https://obsidian.md). An autolink:
<https://commonmark.org>. A reference-style link: [CommonMark][spec],
defined at the bottom of this note.

## Unresolved links

A link to a note that does not exist anywhere in this vault: [[Nowhere]].
It should render distinctly from a resolved link, and clicking it should
offer to create the note rather than silently doing nothing.

## Duplicate basenames (shortest-path resolution)

Two notes in this vault are both named "Same Name":
`Links/Folder A/Same Name.md` and `Links/Folder B/Nested/Same Name.md`. A
bare link naming just the basename should resolve to the shorter path
(Folder A, one level down) rather than the deeper one: [[Same Name]]. Either
copy can still be reached unambiguously by its full path:
[[Folder A/Same Name]] and [[Folder B/Nested/Same Name]].

## Links to attachments

A wikilink straight to an image file, not a note: [[gradient.png]]. See
[[Image Embeds]] for the same file actually embedded rather than just
linked to.

[spec]: https://spec.commonmark.org "The CommonMark specification"
