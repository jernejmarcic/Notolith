---
tags: [demo, embeds]
---

# Note embeds

What to look for: each embed below should render the target note's content
inline, not just a link to it - the whole note, a single section, or a
single block. Any that do not render fall back to a plain link or nothing.

## A whole note

![[Target Note]]

## A single heading's section

Only the "Details" section of Target Note, not the whole thing:

![[Target Note#Details]]

## A single block

Only the block-anchored paragraph:

![[Target Note#^anchor-block]]

## An embed inside running text

A sentence that embeds a note right in the middle of it: here is the target
![[Target Note#Details]] embedded between two clauses, which should still
read as one paragraph once the embed renders as a block or inline card.
