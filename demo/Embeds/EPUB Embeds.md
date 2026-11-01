---
tags: [demo, embeds, epub]
---

# EPUB embeds

What to look for: an `.epub` embedded with `![[...]]` is a file card, a row
with an icon, the file's name and its size, and a click opens it. The icon is
the book's cover, read from the file off the UI thread: the card is laid out
as a plain card first (52 px tall) and then grows to 96 px when the cover
lands. A book with no cover, or one whose cover does not decode, stays the
plain card. Opening the book needs Settings > Core plugins > "EPUB reader" (on
by default, in a build with the EPUB module); without it the card is always
plain and a click opens the file with the system.

The book is `assets/sample.epub`: a two-chapter EPUB 3 made for this vault (see
`docs/DEMO-ASSETS.md`), with a nav document, a stylesheet and a cover that is the
neovim logo.

## The card

Expect the neovim logo as the cover at the left of the card, then the name
"sample.epub" and its size (about 8 KB):

![[sample.epub]]

## Opening it

Click the card (or open the file from the file tree). The book opens in a tab:
a header with its title and author ("A Very Short Book - Notolith Demo"), a
button for the table of contents, smaller and larger text, find in book
(Ctrl+F), a Paged toggle, and the chapters below drawn by the same engine as
HTML blocks. The contents list "Chapter One: A Beginning" and "Chapter Two: An
Ending"; clicking an entry scrolls to it. Paged mode turns pages with Page
Up/Down, the arrow keys or a click on the margins instead of scrolling.

## Highlights

Select a sentence in the book and right-click: the menu offers "Highlight" in
each of a fixed set of colours, and Copy. A highlighted passage's menu offers
Recolour, "Edit note..." (a box with the quote and your note) and "Delete
highlight". The book file is never written: highlights and notes go
straight into a note beside it, `sample highlights.md` in `assets/`, which
you can read, search and link like any other note, and "Extract highlights"
rewrites it from the highlights shown. Delete that file to start clean.

## Beside other cards

For comparison, a file Notolith has no reader for is a plain card with a
generic icon:

![[color-profile.icc]]
