---
tags: [demo, editor, export, import]
title: Pandoc
---

# Pandoc export and import

What to look for: with the `pandoc` core plugin on (Settings > Core plugins > Pandoc) and pandoc installed,
the palette has "Pandoc: Export as Word (.docx)", `.odt`, `.epub`, HTML, LaTeX, RTF, PowerPoint, Beamer and
"another format...", and "Pandoc: Import a file as a note...". Without pandoc the plugin is listed, off and
greyed; pressing it opens a dialog on how to get pandoc.

## Try

1. Run "Pandoc: Export as Word (.docx)" on this note and open the file. ==This sentence is highlighted==,
   %%this one is a comment and is not in the file%%, and the link to [[Target Note|a note]] is a link.
2. The callout below is a quote that starts with its title; the maths is maths: $e^{i\pi} + 1 = 0$.

> [!tip] A callout
> Its body stays a paragraph of its own.

3. The next section is embedded, so it is part of the file, not a link:

![[Target Note#Details]]

4. Right-click a `.docx`, `.odt`, `.epub`, `.html`, `.rtf` or `.tex` file in the explorer and choose
   "Import with pandoc" (or drag one in from a file manager): a new note appears, its pictures copied to the
   attachment folder, never over an existing note.

See [[Export to PDF]] for a PDF and [[Publish]] for the vault as a website. The details, and what is lost, are
in `docs/PANDOC.md`.
