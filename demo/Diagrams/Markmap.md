---
tags: [demo, diagrams, markmap]
---

# Markmap mind maps

What to look for: each ` ```markmap ` fence below is drawn as a mind map, the
way the Obsidian markmap plugin and markmap.js.org draw one: the root at the
left, the headings and list items of the fence as a tree to its right, a
coloured curved branch for each subtree, and the node text with its inline
Markdown (**bold**, *italic*, `code`, links). Notolith lays the map out and
draws it itself - no web view, no JavaScript, no tool to install - a moment after
the note opens, off the main thread. The caret inside a fence shows its source,
so it can be edited, and the map comes back when the caret leaves.

**Click the small circle at the end of a node** to fold or unfold its branch.
That is only how the map is shown: nothing is written to the note, and it
resets when the fence's text changes. In reading view the circles work too. The
map follows the theme (try the light and the dark scheme); a map wider than the
text column is scaled down to fit it. Publish and PDF export draw the map from
the fence's own text (folds from the source, not from your clicks), in the
theme of the output: a PDF is always light, on white paper.

The switch is Settings > Core plugins > "Mind maps"; off, a fence is an ordinary
code block, on screen, in a published site and in a PDF.

## Headings and lists

Headings nest by level; a list under a heading hangs from it, nested by
indentation:

```markmap
# Notolith
## Writing
- Live preview
  - **bold**, *italic*, `code`
  - [links](https://example.com) and [[Feature Tour|wikilinks]]
- Source mode
## Organising
- Folders and tags
- Links and backlinks
  - Graph view
## Publishing
1. Static site
2. PDF export
```

## Several top-level items

With no single root, the map gets an empty one, as in markmap.js:

```markmap
- Read
  - Papers
  - Books
- Write
  - Notes
- Build
```

## Options and folding

The frontmatter of the fence takes markmap's options `colorFreezeLevel` (levels
below this keep the colour of their ancestor on it; 1 is a single colour),
`initialExpandLevel` (the level at which branches start folded: 1 folds even the
root, 2 shows the root and its children) and `maxWidth` (pixels before a
node's text wraps). Levels count from 1 at the root, as in markmap.js.
`<!-- markmap: fold -->` after a node folds it to begin with; `foldAll` folds
everything below it as well.

```markmap
---
markmap:
  colorFreezeLevel: 2
  maxWidth: 260
---
# Options
## Colour
- A branch keeps the colour of its ancestor at the freeze level, so a long outline stays readable
  - deeper
    - and deeper
## Wrapping
- A node with a long sentence in it wraps at the width the fence asks for instead of running on and on across the page
## Folded to begin with <!-- markmap: fold -->
- Click the circle to open this branch
  - and the one inside it
```

## The whole note

Command palette > "Show as mind map" draws the whole note you are in - headings,
lists and text, but not its code blocks or frontmatter.
