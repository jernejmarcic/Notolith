---
tags: [demo, formatting, lists]
---

# List display

What to look for: how lists are drawn and continued in live preview. The note's
text is never changed by these settings, only how it is drawn. They are in
Settings > Editor: "Numbered lists", "Number ending", "Bullet glyphs" and
"Nested bullets".

## Numbers

1. First
   1. First child
   2. Second child
      1. Grandchild
2. Second
3. Third

With "Numbered lists" set to Hierarchical this draws as 1., 1.1., 1.2., 1.2.1.
and 2., 3. instead of restarting at 1 in each sub-list. "Number ending" changes
what follows a drawn number: `.`, `:`, `)` or any short text.

## Bullets

- A dash item
* A star item
+ A plus item
  - A nested dash

"Bullet glyphs" maps each of `-`, `*` and `+` to a dot, ring, square, diamond,
dash, arrow, star, check, turn arrow, or the character as typed. With "Nested
bullets" on, a nested bullet is drawn as a turn arrow whatever its marker.
Put the caret on a bullet and the real marker shows.

## Keys on a list item

- Enter continues the list with the same marker (a task becomes a new `- [ ]`,
  and it works inside a quote or callout). Enter on an empty item steps out
  of the list.
- Tab indents the item, Shift+Tab outdents it, Backspace at the start of an
  item's text takes the marker off. Numbers renumber as you go.
- Shift+Enter breaks the line inside the item with no new bullet.
- Ctrl+Enter cycles a task's state, see [[Lists]].
