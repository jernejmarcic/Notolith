---
tags: [demo, query, search]
---

# Search Samples

What to look for: nothing renders specially here. This note is the corpus the
queries in [[Search Operators]] run against: paragraphs, list items, a callout
and a code fence built so that `block:`, `meta:` and `/regex/` give different
answers. Edit it and the query results there update.

## Paragraphs

Saffron and cardamom share this paragraph.

Saffron appears here without its neighbour.

Cardamom turns up here, in a different block from the saffron above.

## Lists

- Turmeric and ginger share this item, with a nested item below it.
  - Paprika is nested, so it is a block of its own
- Turmeric alone in the next item.

## A callout

> [!note] Pepper and salt
> A callout is one block from its title line on, so pepper and salt match
> together here.

## A code fence

```text
- cumin is on a line that looks like a list item
# anise is on a line that looks like a heading
```

## Fennel

Seeds are in the paragraph under the heading, not in the heading line.

## Meta notes

She left early [check the date] and never came back. The timeline [too
vague?] needs work.
