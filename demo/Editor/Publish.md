---
tags: [demo, editor, publish]
publish: true
---

# Publish

What to look for: "Publish: Export the vault as a website" in the command
palette (Settings > Core plugins > "Publish", on by default) writes the vault,
or only the notes marked `publish: true`, as a static website you can open from
disk or upload anywhere. Nothing is uploaded by Notolith and the vault is
never touched: every read goes through the index, every write goes into the
output folder.

## The dialog

The command opens one dialog:

- **Folder**: where the site is written, with a Browse button. A folder outside
  the vault, or a subfolder of it; never the vault root itself, which is
  refused ("refusing to publish into the vault root itself"). Publish is
  disabled while it is empty.
- **Site name**: shown on every page; empty means the vault's name.
- **Home page**: a note of the vault (completion lists the vault's notes) that
  becomes `index.html`, or empty for a generated list of every published note
  by folder. A name that is not a note disables Publish.
- **Notes**: "Only notes with publish: true" (the first time: Obsidian
  Publish shares nothing until it is picked) or "Every note, except
  publish: false".
- **Theme**: dark, light, or both as the reader's system prefers (one
  stylesheet, no script, no toggle).

The choices are remembered per vault, outside it. The status bar says
"Publishing..." and then "Published N page(s)". Publishing again rewrites only
what changed and removes pages that are no longer published.

## Marking notes

This note has the property `publish: true` (look at its properties block), so
it goes out in the first mode. Its two neighbours decide the other mode:

- [[Publish Example]] has `publish: true`, so it goes out either way.
- [[Publish Private]] has `publish: false`, so it is left out of both modes;
  every note with no `publish` property goes out only in "Every note".

To check: publish to an empty scratch folder in "Only notes with publish: true"
mode and open `index.html`: the site should hold this note, Publish Example,
and the notes of the vault marked the same way, and not Publish Private. Then
publish again with "Every note": every note is there except Publish Private.
The home page choice of "Publish Example" makes it `index.html`.

## What the pages are

- Each note is an `.html` page; wikilinks between published notes are links, a
  link to an unpublished note is drawn as plain (muted) text, not a link.
- Images, audio, video and PDFs a published note embeds are copied beside it.
- Mermaid, PlantUML, TikZ and Graphviz fences are the pictures the editor
  already drew (embedded as `<img>` with a `data:` URI, never inlined, so a
  picture runs no script); a fence never drawn in the editor is its code. Draw
  the diagram notes first if you want them as pictures.
- Code blocks keep their colours as classes (`.tok-keyword`), a tab group
  becomes a strip that needs no JavaScript, and footnotes link to their
  definition and back.

A note outside the chosen mode never leaves the vault: its text is not even
read.
