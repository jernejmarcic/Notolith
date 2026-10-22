---
tags: [demo, workspace, files]
---

# Files and the explorer

What to look for: the left sidebar is the file explorer, and everything that
creates, renames, moves, deletes or hides a file. Try these on a scratch note,
not on the demo notes.

## The tree

- A note shows without `.md`; any other file shows without its extension and
  carries it as a small tag at the right (`BASE`, `CANVAS`, `PDF`). Look at
  `Library/` and `Embeds/assets/`.
- The header has a sort menu (name A to Z or Z to A, modified new to old or
  old to new) and "Collapse all". The choice is kept per vault.
- Settings > Files and links > "Hidden file types" leaves a file type out of the
  tree. Search and links still see it.

## The context menu

Right-click a file or folder: "New note", "New folder", "Rename", "Move to...",
"Make a copy", "Delete", "Open in default app" and "Show in system explorer".
"Move to..." is also a command, "Move file to another folder".

- Rename or move a note that others link to, and the links follow when Settings
  > Files and links > "Update links on rename" is on (it is, in this vault's
  `app.json`: `alwaysUpdateLinks`). Make two scratch notes, link one from the
  other, rename the first, and open the second.
- "Delete" asks first ("Confirm before deleting") and goes where Settings >
  Files and links > "Deleted files" says: the system trash, the vault's
  `.trash`, or gone for good.
- "New note" lands where "Default location for new notes" says: the vault root,
  the open note's folder, or one chosen folder.

## Icons and the inline title

- Settings > Appearance > "Icons on files and folders", then "Change icon..." in
  the context menu, gives a file or folder an icon or one character. The choice
  is kept outside the vault.
- Settings > Appearance > "Show inline title" puts the note's name above its
  text as an editable line. Edit it and press Enter: the note is renamed and
  its links updated, the same as in the tree. Escape puts the old name back.

## Attachments and links

`app.json` sets `attachmentFolderPath` to `Embeds/assets`, so a pasted or
dropped image goes there. It also sets wikilinks (`useMarkdownLinks` false),
the style [[Links and References]] shows.
