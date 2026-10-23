---
tags: [demo, embeds]
---

# CSV and TSV embeds

What to look for: each file below should show as a table - the first line as
the header row, the same borders, fonts and colours as a Markdown table -
rather than as a listing of commas. A moment of "Loading..." first is the
file being read off the editing thread. Turn it off under Settings > Core
plugins > CSV embeds and they become plain listings again.

## A CSV with quoted fields

A comma, a doubled quote and a line break inside quotes all stay in their cell:

![[planets.csv]]

## A semicolon CSV

European Excel writes `;` and a decimal comma; the delimiter is taken from the
first line:

![[prices-eu.csv]]

## A TSV

![[moons.tsv]]
