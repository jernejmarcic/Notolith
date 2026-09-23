---
tags: [demo, embeds, maths]
---

# TeX embeds

What to look for, per `TODO.md`'s "afternoon: embeds" entry ("if a .tex
file is linked it should be rendered in the inline window"): a `.tex` file
containing only maths should render through MicroTeX exactly like a `$$...$$`
block; a whole compilable document should be compiled off the UI thread by
whatever `pdflatex` is on the machine (no shell escape, a time/memory cap,
cached until the file changes) and then shown with the same page arguments
and layouts as a PDF embed. As of this build this is not wired up yet
(`grep`-ing the editor and markdown libraries for `.tex` handling turns up
nothing) - both embeds below are expected to fall back to a plain link or
the source-file panel; say in the report which one actually happened.

## Maths alone

![[maths-only.tex]]

## A full document

Compiles cleanly with `pdflatex full-document.tex` and no packages beyond
`amsmath` - verified by hand while building this vault:

![[full-document.tex]]

## The same document, with PDF-style page arguments

Once `.tex` compilation lands, this should behave like a PDF embed of the
compiled output:

![[full-document.tex#page=1]]
