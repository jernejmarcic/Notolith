---
tags: [demo, editor]
---

# Editing commands

What to look for: the keys and menus for changing text. Work in the scratch
area at the bottom, in live preview. Every command is in the palette (Ctrl+P)
and can be rebound under Settings > Hotkeys.

## Formatting

Select a word, then:

- Ctrl+B bold, Ctrl+I italic, Ctrl+Shift+H highlight.
- "Toggle strikethrough", "Toggle code", "Toggle inline math" and "Toggle
  comment" have no key; run them from the palette or from the right-click menu,
  under Format.
- Ctrl+K "Insert Markdown link". With nothing selected a format applies to the
  word under the pointer.
- Settings > Editor > "Auto-pair brackets and markdown": typing a bracket or a
  quote closes it, and with a selection wraps it.

## Lines and search

- Alt+Up and Alt+Down move the line (or lines) up and down.
- "Duplicate line" is Ctrl+Shift+D (this vault rebinds that key to the daily
  note, so run it from the palette here).
- Ctrl+F finds in this note (reading view too), Ctrl+H finds and replaces.
- Ctrl+Enter toggles a task box, see [[Lists]].

## Link completion

Type `[[` and a list of note names opens; any part of a name matches.

- `[[Target Note#` lists that note's headings, `[[#` the headings of this one.
- `[[Target Note#^` lists its block ids (`^id` markers).
- A note's `aliases` show as "alias -> Note"; picking one inserts
  `[[Note|alias]]`. [[Welcome]] has the alias "Demo Welcome".
- `![[` completes the same way, for embeds.

## Right-click menus

- On text: Cut, Copy, Paste, Format, Add link, Checkbox state, and (when their
  switches are on) Look up, Search for and Search the web for the word, see
  [[Spellcheck and Look Up]].
- On a callout: Edit block source, Copy, Change type (the 13 built-in types and
  every `callouts.ini` section) and Remove callout. Click a callout's title to
  edit it in place. Try on [[Callouts]].
- On a table: the table menu, see [[Table Editing]].

## Saving

Settings > Files and links > "Save automatically" saves two seconds after you
stop typing, and on leaving a note or closing a tab. With it off, Ctrl+S is the
way. A note that is not valid UTF-8 is never saved without asking.

## Scratch area

Type here: 

- a list item

Some text to bold, link and move around.
