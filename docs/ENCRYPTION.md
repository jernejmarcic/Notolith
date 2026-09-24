# Encrypted notes

A note can be kept encrypted on disk and read only in this app, with a
password. Its text is decrypted in memory when the note opens and never
written anywhere in plain form. `libs/ext/crypt`, behind
`NOTEAPPC_WITH_ENCRYPTION` (on when libsodium is found), and the "Encrypted
notes" switch in Core plugins.

## Using it

- **Encrypted notes: Encrypt this note** turns `Name.md` into `Name.mdlock`.
  The first time, it asks for a new password (twice, at least 8 characters).
  The plain file and its file recovery snapshots are deleted.
- **Encrypted notes: New encrypted note** makes an empty `Untitled.mdlock`.
- **Encrypted notes: Decrypt this note** turns it back into a plain `.md`.
- **Encrypted notes: Lock encrypted notes** saves every open encrypted note,
  closes it, and forgets the password.
- **Encrypted notes: Change password** re-encrypts every `.mdlock` in the
  vault. Nothing changes unless every one of them opens with the current
  password first.
- `[[Name]]` links keep working after a note is encrypted.

**When the password is asked** (Settings, Files): by default the first time
an encrypted note is opened; with "Unlock encrypted notes on startup", when
the vault opens. Either way it lasts until the notes are locked, the app
closes, another vault opens, or nothing is typed or clicked for the minutes
set in "Lock encrypted notes after" (15 by default, 0 never).

The password is not stored anywhere. Forget it and the notes are gone.

## The file

    "NAPCLOCK"  8   magic
    version     1   1
    algorithm   1   1 = Argon2id v1.3 + XChaCha20-Poly1305-IETF
    opslimit    4   Argon2id passes, little endian
    memlimit    4   Argon2id memory in KiB, little endian
    salt       16   Argon2id salt
    nonce      24   random for every save
    body        *   the note's UTF-8 bytes encrypted, then a 16-byte tag

The whole header is the associated data, so changing any byte of the file,
header included, makes it refuse to open. New files use libsodium's
MODERATE Argon2id (3 passes, 256 MiB): 155 ms on starbase-1, once per
unlock, since every note of a vault shares one salt and so one key. The
params are in each file, so they can be raised without breaking old notes.

Chosen over Meld Encrypt's `.mdenc` (PBKDF2-SHA512, AES-256-GCM, readable in
Obsidian with that plugin) by Jernej on 2026-09-24: Argon2id resists GPU
password guessing far better than PBKDF2. The price is that only noteAppC
opens these files.

## What never sees the plaintext

- The disk: saves write ciphertext atomically (`Vault::writeBytes`).
- File recovery: no snapshot is kept (`Snapshots::record` refuses a
  `.mdlock`), and encrypting a note deletes the plain one's snapshots.
- Note history: not recorded for an encrypted note.
- The index and search: an encrypted note is indexed by its name only, so
  its links, tags and words are not searchable and it shows no backlinks.
- Plugins: a plugin's code block in an encrypted note is not handed to the
  plugin, and a plugin reading the selection gets nothing (the AI plugin
  would otherwise send it to an API). `readNote` for plugins reads `.md` only.
- Notebooks: code blocks do not run in an encrypted note, since their output
  is cached on disk.
- Page preview and embeds show nothing of it.
- While unlocked the process cannot be core dumped or ptrace-attached
  (`PR_SET_DUMPABLE`), and the password and keys sit in libsodium's locked,
  guarded memory, wiped on lock.

## What it cannot do

- **Swap.** The decrypted text lives in the editor's ordinary memory, which
  the system may swap to disk. Use encrypted swap (or none) if that matters.
- **Copies made before encrypting.** The note history (git), backups, disk
  snapshots (btrfs), other synced devices and a sync server's history keep
  whatever they already had of the plain note. On an SSD a deleted file's
  blocks are not guaranteed to be overwritten.
- **Attachments.** Only the note's text is encrypted; an image pasted into an
  encrypted note is saved as a normal file.
- **The file name** is visible, as with any file.
- **Links inside an encrypted note** are not rewritten when a linked note is
  renamed, since its text cannot be read without the password.
