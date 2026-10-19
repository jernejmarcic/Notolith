---
tags: [demo, editor, meta-notes]
---

# Meta notes and bare brackets

What to look for: two related colourings of `[square brackets]`, both
display only (the file is plain text and Obsidian shows it as written).

A bare bracket, `[like this]` where the brackets are not a link, a footnote or
the like, is drawn in the link colour, with the brackets kept. It is ordinary
syntax colouring, on for everyone, and has no setting.

A meta note is a writer's note to themselves, such as `She left early [check
the date] and never came back.` It is drawn in its own colour (orange unless
the theme sets `--meta-note-color`), brackets kept and never hidden. Meta
notes are **off by default**: turn them on in Settings > Editor > "Meta notes".
With them on a `[run]` is a meta note and no longer just a bare bracket, the
meta colour winning; the right sidebar gets a Meta notes tab; the status bar
counts them; `Alt+]` and `Alt+[` jump between them; and the search field
`meta:` finds them. Off, none of that exists and nothing looks for them.

This note is built so that, with the setting on, the status bar says "8 meta
notes" and the Meta notes tab lists them grouped under their headings.

## In prose

She left early [check the date] and never came back. The scene goes on for
another page [too long?] before anyone mentions the letter. Meta notes can
sit anywhere in a line, at the end of one [cut this paragraph], or inside
formatting like **bold [rephrase] text**.

## A note across a line break

A run may cross a soft line break, but not a blank line. This one should be
drawn as one run over two lines [remember to
check the timeline here] and carry on.

## Bare brackets that are not meta notes

Each of these is not a bare bracket (and so not a meta note) and should not be
coloured as one: a wikilink [[Target Note]], a link [with text](https://example.com),
an image link `![alt](image.png)` in a code span, a footnote reference[^1], a
reference link [text][ref], inline code `[in code]`, inline maths $[a, b]$, an
escaped \[bracket\], a bare URL https://example.com/[x], and a task box:

- [ ] a task whose box is not a bracket
- [x] a finished one

[ref]: https://example.com/ref
[^1]: The footnote's own bracket is not a bare bracket either.

## Nesting and unclosed brackets

Brackets nest: [outer [inner] still outer] is one run, the outermost, drawn as
a whole. An unclosed [ bracket is not a run at all and is drawn as plain
text.

## Jumping between them

`Alt+]` selects the next meta note after the selection, with its brackets, and
`Alt+[` the previous one; past the last it wraps to the first. The status bar
says "Meta note 2 of 8" and, after a wrap, "(wrapped around)". Backspace on
the selection deletes it in one stroke; an arrow key puts the caret inside to
edit it; typing replaces it. With no meta notes it says "No meta notes in this
note". The commands are "Meta notes: Next meta note" and "Meta notes:
Previous meta note" in the command palette.

## Deleting one

Right-click inside a run: "Delete meta note" removes it and one neighbouring
space, as one undo step. Try it on [delete me] in this sentence: the result
should read "Try it on in this sentence" with a single space, not two.

## The Meta notes pane

The tab with the brackets icon in the right sidebar (after Outline) lists
this note's meta notes in document order, grouped under the heading above each
(a heading with none is not listed). Each row is two lines: the text of the
run, then its line number and the words around it. The one the caret is in is
highlighted; Enter or a click selects it in the editor; the filter box narrows
the rows. The Vault switch beside the filter lists every note in the vault that
has meta notes instead, one row each with its count; picking one opens it at
its first meta note. The status bar's "N meta notes" is a button that opens the
pane.

## Other delimiters

Settings > Editor > "Meta note delimiters" sets the opening and closing
strings, 1 to 3 characters each; the same string on both sides, such as `;;`,
pairs them left to right. Markdown's own characters (``* _ ~ = ` $ % # |``) are
refused with a message. After choosing `;;` on both sides, ;;this one;; is a
meta note and the [square brackets] in this sentence go back to being bare
brackets. Change them back to the square brackets to match the counts above.

## Word counts

Settings > Editor > "Exclude text between characters" leaves text between two
delimiters (the meta note ones when the fields are empty) out of the status
bar's word and character counts, so the words of a meta note are not counted
as part of the draft.
