---
tags:
  - demo
  - tags-note
---

# Tags

What to look for: the tags pane (right sidebar) should list `demo`,
`tags-note`, `topic`, `topic/sub` (nested under `topic`), `status/active`
and `status/review` (both nested under `status`) - but **not** `#fake-tag`,
which sits inside a code block below and must be excluded from both the
pane and search.

## Inline tags

A paragraph with an inline tag: #topic. A nested tag, which should appear
under `topic` in the tags pane as a child: #topic/sub. Two more nested under
a different parent: #status/active and #status/review.

## A tag that must not count

The following code block contains something that looks like a tag but is
inside a fence, so it must be inert - the parser must not scan the contents
of a code block for tags:

```text
this is not a real tag: #fake-tag
```

A `#fake-tag` inline-code span should not count either: `#fake-tag`.

## Frontmatter tags

This note's own `tags:` list in the frontmatter (`demo`, `tags-note`) counts
the same as an inline `#tag` - both are the same tag namespace, just written
in two different places.
