---
title: Welcome to the noteAppC demo vault
aliases:
  - Demo Welcome
  - MOC
tags:
  - demo
  - moc
cssclasses:
  - demo-welcome
---

# noteAppC demo vault

This is a small, hand-built vault (kept under 5 MB, see `demo-assets.md` in
the project root next to `demo/` for where every binary file came from) that
exercises one feature area per note. Every note says, near the top, what it
demonstrates and what a correct render looks like, so a regression is
something you can *see* rather than something you have to already know to
spot. Open a note in live preview, then toggle reading view
(`markdown:toggle-preview`), and compare.

This vault is also meant to open cleanly in real Obsidian - nothing here uses
syntax Obsidian does not already understand, except where a note says
explicitly that a feature is noteAppC's own extension (callout/code-block
styling via `callouts.ini`/`codeblocks.ini`, documented in
`docs/STYLING.md`).

Okay so 
## Map of contents

### Text and structure
- [[Basic Syntax]] - emphasis, strikethrough, highlight, code, escapes,
  comments, footnotes, rules, line breaks, entities.
- [[Headings and Outline]] - all six heading levels, for the outline pane.
- [[Long Note]] - a few hundred lines, for scroll and readable-line-length.
- [[Lists]] - bullets, numbered, nested, tasks, indentation guides.
- [[Tables]] - alignment, inline formatting in cells, a wide table.

### Code and callouts
- [[Code Blocks]] - languages, an unknown language, a long line, indented code.
- [[Implicit Code]] - code a note's properties add to every runnable block of its language.
- [[Callouts]] - all 13 built-in types and aliases, folding, nesting,
  titles, a custom type, noteAppC's own inline/`:::` styling.

### Maths
- [[Maths]] - inline and display maths, hard formulas, a malformed one.

### Links and structure
- [[Links and References]] - wikilinks, aliases, heading/block links,
  Markdown links, external links, unresolved links, duplicate basenames,
  attachment links.
- [[Target Note]] - the note most of the link tests point at.
- [[Hub]] and its linkers - backlinks and unlinked mentions.
- [[Tags]] - inline, nested, frontmatter, and one that must not count.
- [[Properties]] and [[Malformed Frontmatter]] - every property type.

### Embeds
- [[Note Embeds]], [[Image Embeds]], [[PDF Embeds]], [[Media Embeds]],
  [[Source Embeds]], [[Tex Embeds]], [[Other Embeds]] - one note per embed
  kind;.

### Query, bases and raw blocks
- [[Query Blocks]] - ` ```query ` embedded search.
- [[Base Blocks]] - inline ` ```base ` YAML.
- `Library.base`, in `Library/`, over the [[Books]] collection.
- [[HTML Blocks]] - raw HTML through litehtml.

### Canvas
- `Board.canvas`, in `Canvas/` - cards, groups, edges, a valid JSON Canvas 1.0 file.

### Daily notes and templates
- `Daily/` - four consecutive daily notes, `.obsidian/daily-notes.json`.
- `Templates/` - `{{title}}` / `{{date}}` / `{{time}}` templates.

### Unicode and odd filenames
- [[Unicode and Scripts]] - CJK, RTL, combining marks, long words.
- a file with spaces, and one with non-ASCII characters, linked from there.

### History
- [[Note History]] - what to do to see git-backed note history and recovery.

### Every feature
- [[Feature Tour]] - every supported feature, each linked to the note that
  shows it and says how to try it.

### Newer features
- [[Newer Features]] - a map of the notes for what came after this vault: diagrams,
  code, editor features, queries and embeds.

## Config exercised by this vault

`.obsidian/` carries `app.json`, `appearance.json`, `core-plugins.json`
(every core plugin, object form, all on), `graph.json` (colour groups and
filters matching this vault's tags), `daily-notes.json`, `templates.json`,
`bookmarks.json`, `workspace.json` and `workspaces.json` (two named
layouts), and `hotkeys.json` (a couple of rebinds).
Ok imma try typing something
seems pretty smooth now. pretty GOATED
Yes that is IT, its going great for n
