---
tags: [demo, editor, daily, templates]
---

# Daily notes and templates

What to look for: two plugins that make notes from a pattern. The folders
`Daily/` and `Templates/` hold this vault's samples.

## Daily notes

`.obsidian/daily-notes.json` says folder `Daily`, format `YYYY-MM-DD`, template
`Templates/Daily Note`.

- "Daily notes: Open today's daily note" opens `Daily/<today>.md`, making it
  from the template if it is missing. Delete the result afterwards.
- "Open previous daily note" and "Open next daily note" step by one day; see
  [[2026-09-21]] and its neighbours for four existing days.
- Ctrl+Shift+D runs the first command in this vault (`hotkeys.json`).
- The date format is under Settings > Files and links > "Daily note date format".

## Templates

`.obsidian/templates.json` says folder `Templates`.

- "Templates: Insert template" asks which template to use and inserts it at the
  caret in a scratch note. [[Meeting Notes]] shows `{{title}}`, `{{date}}`,
  `{{time}}` and the `{{date:YYYY-MM-DD}}` and `{{time:HH:mm}}` forms filled in.
- Settings > Files and links > "Template date format" and "Template time format"
  are the formats a bare `{{date}}` and `{{time}}` use.
