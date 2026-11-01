---
tags: [demo, history]
---

# Note history and file recovery

What to look for: two different, related features.

## Git-backed note history

Every save of a note is committed as a git diff outside the vault (no
dotfile inside it; 0.093 ms a
save). To see it on this note:

1. Edit this paragraph - add a sentence, save.
2. Edit it again, differently, save again.
3. Right-click the note (or its tab) and choose "Note history: Show this
   note's history".
4. The history window should list both saves, each showing a diff against
   the version before it, and offer to restore an older one

## File recovery snapshots

A separate, older mechanism: a snapshot on save at most every 5 minutes per
note, kept for 7 days, under `$XDG_DATA_HOME/notolith/recovery`. To see it:

1. Make an edit and wait past the 5-minute snapshot interval (or make
   several edits across a longer session).
2. Open the file-recovery window (search the command list for "recovery").
3. Find this note, read an older version, "Show changes" for a line diff
   against the note as it stands now, or restore it outright.

Both features are switchable off independently in Settings > Core plugins
(`note-history`, `file-recovery`).
