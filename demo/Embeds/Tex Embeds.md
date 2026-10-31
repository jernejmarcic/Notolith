---
tags: [demo, embeds, maths]
---

# TeX embeds

What to look for (a linked .tex file is rendered in the inline window): a `.tex` file
containing only maths should render through MicroTeX exactly like a `$$...$$`
block; a whole compilable document should be compiled off the UI thread by
whatever `pdflatex` is on the machine (no shell escape, a time/memory cap,
cached until the file changes) and then shown with the same page arguments
and layouts as a PDF embed. If `.tex` embeds are not available in a build, both embeds below fall back
to a plain link or the source-file panel.

## Maths alone

![[maths-only.tex]]

## A full document

Compiles cleanly with `pdflatex full-document.tex` and no packages beyond
`amsmath` - verified by hand while building this vault:

![[full-document.tex]]

## The same document, with PDF-style page arguments

This should behave like a PDF embed of the
compiled output:

![[full-document.tex#page=1]]
