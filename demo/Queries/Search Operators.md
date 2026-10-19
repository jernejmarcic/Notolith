---
tags: [demo, query, search]
---

# Search operators

What to look for: each ` ```query ` block below should show "Searching..." for
a moment and then its result, a list of notes with a match count and a
highlighted snippet, or "No results for" the query. Every query ends with
`file:Samples` so it only looks at [[Search Samples]], the corpus built for it;
without that, this note would match its own query text. The result is stated
under each block. The grammar is Obsidian's, with differences where this note
says so (`docs/SEARCH.md`), and `meta:`, which is Notolith's own.

## block:

`block:(a b)` needs both words inside one block. A block is a paragraph, one
list item (a nested item on its own), a heading line, or a callout from its
title line on. Both words are in one paragraph, so Search Samples is listed:

```query
block:(saffron cardamom) file:Samples
```

The same two words are in different paragraphs below it, and a plain query
finds the note either way:

```query
cardamom turmeric file:Samples
```

but as a block query they are never together: "No results".

```query
block:(cardamom turmeric) file:Samples
```

One list item holds both words, so it matches:

```query
block:(turmeric ginger) file:Samples
```

A nested item is a block of its own, apart from the item it sits in: ginger is
in the parent and paprika in the child, so "No results".

```query
block:(ginger paprika) file:Samples
```

A callout is one block from its `> [!note]` line on, so this matches:

```query
block:(pepper salt) file:Samples
```

A heading is a block by itself: "fennel" is in the heading and "seeds" in the
paragraph under it, so "No results".

```query
block:(fennel seeds) file:Samples
```

List markers and headings inside a fenced code block are code, they do not
split it, so both words in the fence are one block and this matches:

```query
block:(cumin anise) file:Samples
```

## /regex/

A pattern between slashes is PCRE (Qt's regular expressions, not JavaScript's),
run over the note's whole text, front matter included. Matches `cardamom` and
`caradamom` alike, so Search Samples is listed:

```query
/car(a)?d?amom/ file:Samples
```

`^` and `$` mean the start and end of the *note*, not of a line. The heading
`## Fennel` is not at the start of the note: "No results".

```query
/^## Fennel/ file:Samples
```

Write `(?m)` to make `^` a line start, and now it is found:

```query
/(?m)^## Fennel/ file:Samples
```

The note itself starts with its front matter, so this matches the opening
`---`:

```query
/^---/ file:Samples
```

There are no flags after the closing slash. `/saffron/i` is the regular
expression `saffron` AND the word `i`, and the note has no word `i`, so "No
results"; write `(?i)` inside instead:

```query
/saffron/i file:Samples
```

```query
/(?i)SAFFRON/ file:Samples
```

Case is ignored unless `match-case:` says otherwise, for regular expressions as
for words; with it, `SAFFRON` is not found:

```query
match-case:/SAFFRON/ file:Samples
```

## meta:

Not in Obsidian. `meta:word` needs the word inside one meta note, a run in the
editor's meta note delimiters (`[like this]` by default). Meta notes are
**off by default**: turn on Settings > Editor > "Meta notes" first (see
[[Meta Notes]]). With it off, every `meta:` query matches nothing and says "No
results". With it on, `[check the date]` in Search Samples is a meta note, so
this lists the note:

```query
meta:date file:Samples
```

The word "timeline" is outside any meta note ("too vague?" is the meta note
there), so this is "No results" even with the setting on:

```query
meta:timeline file:Samples
```

A space after the colon ends the field: `meta: date` is `meta:` AND `date`, which
finds the note too, since it has both a meta note and the word.

```query
meta: date file:Samples
```

Bare `meta:` is every note with a meta note and highlights each run whole; `-meta:`
is every note without one. Changing the delimiters re-parses the whole vault once.

## Combining

Operators compose: this wants the two words in one block and the tag:

```query
block:(saffron cardamom) tag:#search file:Samples
```
