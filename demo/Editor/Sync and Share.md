---
tags: [demo, editor, sync, share]
---

# Sync and share

> Not in the default build: `-DNOTEAPPC_WITH_SYNC=ON` and `-DNOTEAPPC_WITH_SHARE=ON` (docs/OFFLINE.md).

What to look for: two features that need a second machine or a server, so this
note says what to expect rather than showing it. Neither does anything until
you ask: with the plugin off nothing is constructed.

## Sync

Settings > Core plugins > Sync. The vault is synced through a server that only
ever stores encrypted copies: it never sees note contents, file names or tags.
Everything works offline; sync is another device catching up.

1. Settings > Core plugins, under Sync: fill in "Sync server" (a URL) and "Sync
   account" (your email). The server is `tools/sync_server/` in the project.
2. "Sync: Sign in" asks for the account password and derives the keys on this
   device.
3. "Sync: Sync now" pushes and pulls. "Sync: Forget this device's keys" signs
   this device out.

The design, key hierarchy and conflict rules are in `docs/SYNC.md`.

## Share a note

Settings > Core plugins > Share a note. Live editing of one note with other
people, peer to peer, end-to-end encrypted. Needs two devices.

1. Open a scratch note and run "Share: Share this note". A dialog shows an
   invitation code with Copy and "New invite" (a new code voids the old one).
2. On the other device run "Share: Join a shared note" and paste the code. The
   copy is written to `Shared/<title>.md`.
3. The owner approves the new device after comparing the safety number shown on
   both. Each person's caret shows in the other's editor as a coloured bar with
   a name; the status bar says "Shared - N here"; the tab gets a mark.
4. "Share: Who has access to this note" lists members and lets the owner remove
   one. "Share: Stop sharing this note on this device" ends it here.

An encrypted note ([[Encrypted Notes]]) cannot be shared. Details: `docs/SHARE.md`.
