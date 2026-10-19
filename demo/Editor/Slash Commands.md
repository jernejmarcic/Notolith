---
tags: [demo, editor, slash-commands]
---

# Slash commands

What to look for: typing `/` at the start of a line or after a space opens a
popup of commands, fuzzy filtered by what you type after it. Up and Down move,
Enter or Tab runs the highlighted one, Escape closes it. The same list as the
command palette, with each command's hotkey muted at the right of its row.
This is the Obsidian core plugin "Slash commands", **off by default**: turn it
on in Settings > Core plugins > "Slash commands". With it off, `/` is an
ordinary character and nothing is built.

## Try it

Put the caret at the end of the empty line below, type `/` and then `table`:
the popup should list "Insert table" first (a typed word that is a whole word
of a command's name beats fuzzy matches, shortest name first), then the
other table commands. Enter inserts Obsidian's 2x2 table with the caret in
its first cell. Delete the table afterwards.



Then type `/` and `bold` (the formatting commands), or `/` and `daily` (open
today's daily note). The words you type after the slash narrow the list as
you go; `/insert table` works too, several words are ranked by fuzzy score.

## Where it does not open

A slash that is part of a word is text: type a/b here, or a URL like
https://example.com/path, and no popup appears. Neither does it open inside a
code block, inline code or maths, where a slash is code:

```text
/table is only text in a code block
```

Inline code `/table` is not a command either, and a formula like
$a/b$ is not a trigger.

## Same commands as the palette

Every entry is a command from the command palette (Ctrl+P), so a command that
is off is not in the popup: turn "Daily notes" off in Core plugins and "Daily
notes: Open today's daily note" disappears from both.
