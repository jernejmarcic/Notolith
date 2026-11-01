---
tags: [demo, editor, encryption]
---

# Encrypted notes

> Not in the default build: `-DNOTEAPPC_WITH_ENCRYPTION=ON` (docs/OFFLINE.md).

What to look for: a note can be kept encrypted on disk as `Name.mdlock` and
read only in Notolith, with a password. Its text is decrypted in memory when it
opens and never written anywhere in plain form. This is Settings > Core
plugins > "Encrypted notes" (on by default; it needs a build with libsodium).
There is no encrypted file in this vault: the password is yours, and a file
made here would have a password the vault could not keep a secret. Make one
from a scratch note instead.

## Encrypt this note

1. Open a note you do not mind encrypting: a new scratch note (Ctrl+N) with a
   few lines of text, a heading, a tag and a link in it.
2. Run "Encrypted notes: Encrypt this note" from the command palette.
3. The first time, a dialog asks for a new password, twice, at least 8
   characters. The note becomes `Name.mdlock` in the same folder, the plain
   `.md` and its file recovery snapshots are deleted.
4. Links such as `[[Name]]` to it keep working: they resolve to the
   `.mdlock`.

## Open it later

Open `Name.mdlock` from the file tree or a link. By default the password is
asked the first time an encrypted note opens (Settings > Files: "Unlock
encrypted notes on startup" asks when the vault opens instead). It then lasts
until you lock the notes, close the app, open another vault, or are idle for
"Lock encrypted notes after" minutes (15 by default, 0 never).

## Commands

- "Encrypted notes: New encrypted note" makes an empty `Untitled.mdlock`.
- "Encrypted notes: Decrypt this note" turns the open one back into a plain
  `.md`.
- "Encrypted notes: Lock encrypted notes" saves every open encrypted note,
  closes it and forgets the password.
- "Encrypted notes: Change password" re-encrypts every `.mdlock` in the vault,
  and changes nothing unless all of them open with the current password.

## What an encrypted note does not do

Check these on your scratch note, they are the point of it:

- Search and the quick switcher find it by name only: its words, tags and links
  are not indexed, and it shows no backlinks.
- Code blocks in it do not run (their output would be cached on disk), and
  diagram fences are not drawn (the source would go into the cache).
- No history is recorded for it and no file recovery snapshot is kept.
- Page preview and embeds show nothing of it.
- Plugins, including the AI one, are not handed its text.

## What it cannot hide

The file name, any image pasted into it (attachments are ordinary files),
copies made before it was encrypted (git history, backups, snapshots, other
synced devices), and the decrypted text while it is open, which the system may
swap to disk. The password is stored nowhere: forget it and the note is gone.
The file is Argon2id and XChaCha20-Poly1305, so only Notolith opens it, not
Obsidian.
