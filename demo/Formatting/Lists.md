---
tags: [demo, formatting]
---

# Lists

What to look for: nested bullets should show an indentation guide (a thin
vertical line) down to their parent; task checkboxes should be clickable in
both live preview and reading view; the theme's task states below need
`callouts.ini`-independent styling support for anything past `[ ]`/`[x]` -
if only those two render, the rest is a known gap to report.

## Bullets, nested

- First-level item
- Another first-level item
  - Second level
    - Third level
      - Fourth level, to see how deep the indentation guides go
  - Back to second level
- A first-level item with **bold**, *italic*, `code`, and a [[Target Note|link]] inside it

## Numbered, nested

1. First step
2. Second step
   1. A sub-step
   2. Another sub-step
      1. And a sub-sub-step
3. Third step
4. Fourth step, to confirm numbering does not reset after the nested group

## Mixed nesting

1. Ordered parent
   - Unordered child
   - Another unordered child
     1. Ordered grandchild
2. Ordered parent, item two

## Tasks

- [ ] An open task
- [x] A finished task
- [-] Cancelled (Obsidian's task-states convention, dash)
- [/] In progress (half-filled checkbox in themes that support it)
- [!] Important
- [?] Question
- [ ] A task with **formatting** and a [[Hub]] link inside it
  - [ ] A nested open task
  - [x] A nested finished task

## Loose vs tight lists

A tight list has no blank line between items:
- one
- two
- three

A loose list has a blank line between items, which Markdown renders with
paragraph spacing instead of packed lines:

- one

- two

- three
