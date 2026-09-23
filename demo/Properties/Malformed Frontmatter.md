---
title: Malformed Frontmatter
tags: [demo, properties]
bad-list: [unterminated, "missing quote
nested:
  - one
  - two:
    - inconsistent: indentation
      breaks: yaml
---

# Malformed frontmatter

What to look for: the YAML block above this heading is deliberately broken
(an unterminated quoted string inside `bad-list`, and inconsistent nesting
further down). noteAppC should not crash and should not lose the rest of the
note - at minimum the body below should still render, and the properties
editor should either show nothing for this note or flag the block as
invalid rather than guessing at a partial parse.

The body text after a broken frontmatter block should still be readable
Markdown: **bold**, *italic*, and a [[Target Note|link]] all still work down
here regardless of what happened above.
