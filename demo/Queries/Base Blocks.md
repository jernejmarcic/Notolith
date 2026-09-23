---
tags: [demo, bases]
---

# Base blocks

What to look for: a ` ```base ``` ` block holds the same YAML a `.base`
file does (see `Library/Library.base` for a standalone file using the same
grammar) and should render as a table right in the note. Per `TODO.md`
("Wave 3": "` ```base ``` ` blocks inside notes, on `BaseView`" is still
unchecked as of 2026-09-23), this is not wired up yet - expect this block to
render as a plain unstyled code block rather than a table; say so in the
report.

## Filters, a formula, and a table view

```base
filters:
  and:
    - 'file.inFolder("Library/Books")'
    - 'status != "done"'
formulas:
  ppu: "(price / pages).toFixed(3)"
views:
  - type: table
    name: In progress
    order:
      - file.name
      - status
      - formula.ppu
    sort:
      - property: status
        direction: ASC
```

## Grouped and sorted, a second view

```base
filters:
  and:
    - 'file.inFolder("Library/Books")'
views:
  - type: table
    name: By status
    order:
      - file.name
      - status
      - rating
    groupBy:
      property: status
      direction: ASC
    sort:
      - property: rating
        direction: DESC
  - type: table
    name: Everything
    order:
      - file.name
      - status
      - pages
      - rating
```
