---
tags: [demo, editor, composer]
---

# Note composer

What to look for: Settings > Core plugins > Note composer adds two commands.
Use them on throwaway notes (Ctrl+N), not on the demo notes.

## Extract a selection

1. In a scratch note, write a heading and two lines under it, and select them.
2. Run "Note composer: Extract current selection".
3. A dialog asks for the new note's name, pre-filled from the first heading, else
   the first line. Confirm.
4. The text moves into the new note, in the same folder, and the selection is
   replaced by `[[New name]]`. The setting `noteComposer.replaceWith`
   (`link` or `embed`, in `settings.ini`, no row on the settings page) can make it
   `![[New name]]` instead, which still shows the text.

## Merge a note into another

1. Open a second scratch note.
2. Run "Note composer: Merge current file with another file".
3. Pick the target in the picker ("Merge ... into...").
4. The note's body (not its properties) is appended to the target, links to the
   merged note are rewritten to the target, and the merged note goes where
   "Deleted files" sends it.

A merge refuses a note that is not valid UTF-8, so no bytes are changed.
