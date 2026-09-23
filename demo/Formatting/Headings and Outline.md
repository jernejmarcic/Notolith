---
tags: [demo, formatting]
---

# Level one heading

What to look for: the outline pane (right sidebar) should list all six
levels below as a tree, indented by level, with the heading nearest the
caret shown in bold; clicking any entry jumps the editor to it.

Some body text under the level-one heading, so the outline has something to
skip past.

## Level two heading

Body text.

### Level three heading

Body text.

#### Level four heading

Body text.

##### Level five heading

Body text.

###### Level six heading

This is as deep as Markdown headings go; a seventh `#` would just be a
level-six heading with a literal `#` in its text.

## A second level two, to check sibling ordering

The outline should show this next to the first level-two heading, both
nested under "Level one heading", with "Level three heading" through
"Level six heading" nested under the *first* level-two heading only.

### Back to level three

Confirms the tree resumes nesting correctly after a sibling heading closed
out the deeper levels.
