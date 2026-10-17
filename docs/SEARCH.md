# Search

The search pane (Ctrl+Shift+F, `global-search:open`) takes Obsidian's query
grammar: words (implicit AND), `"phrases"`, `OR` (capitals only), `-negation`,
`( )`, and the fields `path:` `file:` `tag:` `line:` `block:` `section:`
`task:` `task-todo:` `task-done:` `content:` `match-case:` `ignore-case:`,
`[property:value]`, and `/regex/`, plus this app's own `meta:`. A query is parsed by `SearchQuery`
(`libs/index/Search.cpp`), narrowed through the FTS5 table, then each candidate
note is checked against the real text, off the UI thread. This page lists
where the behaviour differs from Obsidian's, so a query copied from there does
not silently mean something else.

## `block:`

`block:(a b)` needs both words inside one block. A block here is:

- a paragraph: a run of non-blank lines;
- each list item on its own, nested items too (a continuation line stays with
  its item); bullets `-` `*` `+` and `1.` or `1)` numbers;
- each ATX heading line (`# x`), alone;
- a callout from its `> [!type]` title line on, and a quoted paragraph ends at
  a line holding only `>` markers, like a blank line;
- list markers, headings and quotes inside a fenced code block are code:
  they do not split it.

Where it still differs from Obsidian: the split is a line scan, not the
Markdown block tree, so a setext heading (`Title` over `=====`), a lazy
continuation line of a quote, a table row and an HTML block are not told apart
from paragraphs; a table is one block per run of non-blank lines.

## `meta:`

Not in Obsidian. `meta:date` needs the word inside one meta note (the editor's
Meta notes setting, by its delimiters, `[like this]` by default); bare `meta:`
is every note with one, and highlights each run whole; `-meta:` is the notes
without. A space after the colon ends the field, so `meta: date` is `meta:` AND
`date`. Runs are indexed only while the setting is on: off, `meta:` matches
nothing. Changing the delimiters re-parses the whole vault once, off the UI
thread. A run that reads as an HTML tag, `<<like this>>` with `<` delimiters,
is HTML and not a run; `<< like this >>` is one.

## `/regex/`

The pattern is PCRE2 (Qt's `QRegularExpression`), not JavaScript's engine.

- The pattern runs over the note's whole text, frontmatter included, so
  `^` and `$` mean the start and the end of the note, not of a line; write
  `(?m)^` for a line start. Inside `line:`, `block:` or `section:` it still
  scans the whole text but keeps only matches that lie within the range
  (`^` therefore only ever matches at the very start of the note).
- No flags after the closing slash. `/x/i` is the regex `x` AND the word `i`.
  Case is ignored unless `match-case:` says otherwise, as for words, in `tag:/x/`, `path:/x/`, `file:/x/` and `[property:/x/]` too; inline `(?i)`, `(?s)`, `(?m)`, `(?x)` work.
  `match-case:` / `ignore-case:` take a field as well: `match-case:tag:/work/`.
- The first unescaped `/` ends the pattern, also inside a class: JavaScript
  reads `/[/]/` as a class holding a slash, here it is a bad pattern. Write
  `[\/]`.
- PCRE syntax JavaScript lacks works: atomic groups `(?>...)`, possessive
  quantifiers, `\h` `\R` `\A` `\z`, `\Q...\E`, recursion, `(?|...)`.
  JavaScript's own `\u{...}` and `/u` Unicode-set syntax do not; `\x{263A}`
  does. Named groups are `(?<name>...)` in both; `\k<name>` is too.
- A regex cannot be turned into an FTS5 term, so a query that is only a regex
  (or an `OR` with one side a regex) tries every note, not an index lookup.
  Inside `path:`, `file:` and `[property:/x/]` the same engine and the same
  rules apply, to the path, the file name or the property value.
- At most 500 matches per note are highlighted.

Both rules are pinned by `tests/test_search.cpp`.

## Quick switcher

Ctrl+O (`switcher:open`) fuzzy-finds a note by path. It is also "find or
create", as in Obsidian: Enter with no match, or Shift+Enter with matches
showing, creates a note named as typed (a trailing `.md` is not doubled) and
opens it. A bare name goes where New note puts one: Settings > Files "Default
location for new notes" (`files.newNoteLocation`: the vault root, the open
note's folder, or `files.newNoteFolder`), read from Obsidian's
`newFileLocation` / `newFileFolderPath` in `app.json`. A name with a `/` is
from the vault root and creates that folder path first. A name that is empty
or holds `# ^ [ ] | : \`, or a `..` folder, is refused in the status bar; an
existing name gets a numeric suffix, like New note. The command palette and the merge picker
share the widget but never create (`QuickSwitcher::setCreateAllowed`).
