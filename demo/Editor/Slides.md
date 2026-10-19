---
tags: [demo, editor, slides]
---

# Slides

What to look for: run "Slides: Start presentation" from the command palette
(Settings > Core plugins > "Slides", on by default) and this note is shown
full screen, one slide at a time, each part between two `---` rules. This
first part is slide 1.

---

## How it works

The presentation starts at the slide the caret is on. Right, Down, Page Down
and Space go forward; Left, Up, Page Up and Backspace go back; Home and End
jump to the first and last slide; Escape ends it and returns to the note
(leaving full screen if the presentation started it). A counter such as
"2 / 7" sits at the bottom right.

---

## The text fills the screen

Each slide is drawn in reading view, at a size that grows with the height of the
screen and never goes below the note's own size. A link, a list and
formatting all work as they do in reading view:

- a bullet with **bold** and *italic*
- a link to [[Welcome]]
- a task list:
  - [x] done
  - [ ] not yet

---

## A slide keeps its blocks whole

A code block or a callout is never split, even when it contains lines that look
like rules:

```python
print("slide")
---
print("this line of dashes is inside the code block, not a slide break")
```

And a callout, which also stays whole:

> [!tip] A callout
> Stays on its slide with its title and its colour.

---

## A picture

![[nvim-logo.png|120]]

---

## What does not become a slide

The properties block at the top of this note is left out. A rule at the very
start or end of a note, or two in a row, makes no empty slide. A diagram fence
(Mermaid, PlantUML, TikZ, Graphviz) is not drawn on a slide: it shows as its
source.

---

## The last slide

Press Escape to leave. The note is exactly as you left it; presenting never
edits it.
