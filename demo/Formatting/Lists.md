---
tags: [demo, formatting]
---

# Lists

What to look for: nested bullets should show an indentation guide (a thin
vertical line) down to their parent; task checkboxes should be clickable in
both live preview and reading view. With the "Checkbox states" core plugin on
(the default), every box in the Tasks list below has its own look: a line for
cancelled, a red X for dropped, a half-filled box for in progress, arrows, a
star and letters; cancelled and dropped text is struck and muted; a click
cycles a state (`[-]` goes back to `[ ]`, `[/]` goes to `[x]`) and a right-click
on the item offers every state under "Checkbox state". The undefined `[q]`
is still a task, drawn as its letter. Off, only `[ ]` and `[x]` are tasks.

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
- [~] Dropped (a red X)
- [/] In progress (half-filled checkbox)
- [>] Forwarded to later (arrow)
- [<] Scheduled (arrow back)
- [!] Important
- [?] Question
- [*] Starred
- [I] Idea (not a task: only a search for `task:` finds it)
- [p] A pro
- [c] A con
- [q] A state nothing defines: still a task, click makes it `[x]`
- [ ] A task with **formatting** and a [[Hub]] link inside it
  - [ ] A nested open task
  - [x] A nested finished task
  - [-] A nested cancelled task

> [!todo] Tasks in a callout
> - [/] Half way
> - [~] Dropped

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
