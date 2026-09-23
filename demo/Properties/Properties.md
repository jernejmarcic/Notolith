---
title: Properties
summary: One of every Obsidian property type, for the properties editor to render.
rating: 4
pageCount: 312
done: false
published: true
started: 2026-09-01
reviewed: 2026-09-23T14:30:00
authors:
  - Jernej
  - The noteAppC project
tags:
  - demo
  - properties
aliases:
  - Property Types
cssclasses:
  - demo-properties
---

# Properties

What to look for: opening this note's properties editor (above the content,
or Settings-gear on a property) should show one control per type - a plain
text box for `title`/`summary`, a number spinner for `rating`/`pageCount`, a
checkbox for `done`/`published`, a date picker for `started`, a date-and-time
picker for `reviewed`, and a list editor (add/remove chips) for `authors`,
`tags`, `aliases` and `cssclasses`. Editing a value in the editor should
write back only that property, byte-exact everywhere else in the file.

## What each property is for

- **text** (`title`, `summary`): a plain string.
- **number** (`rating`, `pageCount`): stored unquoted so YAML parses it as a
  number, not a string.
- **checkbox** (`done`, `published`): YAML `true`/`false`.
- **date** (`started`): `YYYY-MM-DD`, no time component.
- **date & time** (`reviewed`): full ISO 8601 with a time.
- **list** (`authors`): a YAML block list; Obsidian's list property also
  accepts the inline `[a, b, c]` form (see [[Malformed Frontmatter]] for
  what happens when the YAML itself is broken).
- **aliases**: alternate names this note resolves under - see how
  [[Hub]] uses this to catch unlinked mentions of its alias too.
- **tags**: this note's own frontmatter tags, same namespace as an inline
  `#tag`.
- **cssclasses**: extra CSS classes on the rendered note, for a
  theme/snippet to hook into - noteAppC reads it into the rendered note's
  container the same way.

See also [[Library]] and its `Books/` notes for a whole collection of notes
carrying consistent properties, queried by `Library.base`.
