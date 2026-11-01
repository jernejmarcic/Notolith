---
tags: [demo, editor, page-preview]
---

# Page preview

What to look for: hover a link and a popover shows the linked page rendered,
beside the pointer, after about a third of a second. It stays while the
pointer is on the link or on the popover itself (so it can be scrolled and the
links inside it followed), and goes a moment after the pointer leaves both.
Settings > Core plugins > "Page preview" (on by default) switches it off, and
then nothing is tracked.

In reading view (Ctrl+E) hovering is enough. In live preview and source mode
hold Ctrl while hovering, so that moving the pointer across text you are
editing does not throw popovers up. A link to the web (`https://...`) or an
email address has no page, so it never previews; neither does a link to
something that does not exist.

## A note

Hover [[Target Note]]: its whole text, from the top, rendered as in reading
view.

## A heading

The part of a note a heading link names, scrolled into place. Hover
[[Target Note#Details]]: the popover should open at the "Details" section,
not at the top.

## A block

A block link scrolls to the block. Hover [[Target Note#^anchor-block]]: the
popover opens on the paragraph that carries the `^anchor-block` mark.

## A link within this note

A link with no note name means this one. Hover [[#Footnote on previews]] to
see the section at the bottom of this note.

## A long note

Hover [[Long Note]], then move the pointer onto the popover and scroll it with
the wheel; it should not close while the pointer is on it.

## An image

An image link previews as the picture, scaled to fit. Hover
[[gradient.png]].

## A page of a PDF

A PDF link previews as an image of a page, the first or the one named. Hover
[[graphviz-dot-manual.pdf#page=3]] for page 3 of the manual, and
[[graphviz-dot-manual.pdf]] for page 1.

## A link inside a preview

Hover [[Links and References]], then click a link inside the popover: it is
followed as if you had clicked it in the note.

## Something that does not preview

A web link, [an example](https://example.com), and a link to a note that does
not exist, [[A note nobody wrote]], show no popover.

## Footnote on previews

This is the section [[#Footnote on previews]] points at.
