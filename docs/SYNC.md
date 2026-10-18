# Sync

Local-first sync to an optional, untrusted server. The vault is the source of
truth; the server relays and stores encrypted copies so other devices can
catch up. Everything works fully offline and sync is just another device
catching up when it can reach the server.

This is stage 10 of the roadmap. Real-time collaboration (a CRDT per note
body) is later and not built here - see "What is left for the CRDT stage" at
the end. This document is the contract the code in `libs/ext/sync/` and
`tools/sync_server/` implements; where the code and this doc disagree, that is
a bug in one of them.

## Threat model

The server is assumed **hostile-but-available**: it may read, retain,
correlate, reorder-within-what-the-protocol-allows, or refuse to serve
anything it is given, but the protocol must not depend on it doing the right
thing with plaintext, because it never has plaintext. It is not assumed to
mount an active man-in-the-middle against TLS, and it is not assumed to
compromise a device (a device that is compromised has the keys anyway - out of
scope, as for every E2EE system).

What is **in scope**: a server that reads its own database for profit or
under subpoena; a server that swaps ciphertext between two records to see if a
client notices (must be rejected - associated data); a server that replays an
old record to roll a note back (must be rejected - sequence numbers); a
network attacker between client and server (mitigated by TLS when configured,
loopback-only in tests); a stolen laptop with the OS keyring locked (device
identity key is inert without the OS unlocking it); a stolen server disk
(every record and blob is ciphertext, keys never touch it).

What is **out of scope**: a compromised client (it has the plaintext by
definition), a malicious collaborator once collaboration ships (that stage's
problem), traffic analysis by an adversary who can watch the wire over time
(sync's timing and batch sizes are not disguised - see "What the server
learns"), and side channels in libsodium itself (trusted as documented).

## Key hierarchy

Three tiers, exactly as specified, plus the device identity that makes
enrolment work without redistributing the password:

```
password
  |  Argon2id (Standard Notes 004 params)
  v
masterKey (32 B) ---------- never syncs, never written to disk in plain form
  |  crypto_kdf_derive_from_key, three contexts
  |------------------------------------+------------------------+
  v                                    v                        v
loginKey (32 B)                  wrapKey (32 B)          contentKey (32 B)
  authenticates to the server      wraps itemsKeys         keys the content
  (server stores only its                                  address hash for
  Argon2id hash - see below)                                attachment blobs

itemsKey (32 B, random, one active + retired history)
  wrapped by wrapKey for sync, OR delivered to a new device pre-wrapped
  inside a lockbox (below) so that device never needs wrapKey at all
  |  AEAD-wraps
  v
noteKey (32 B, random, one per entity)
  encrypts that entity's journal payload and file content
```

### Deriving masterKey

`crypto_pwhash` with `crypto_pwhash_ALG_ARGON2ID13`, `opslimit = 5`,
`memlimit = 64 * 1024 * 1024` (64 MiB), output 32 bytes. libsodium's Argon2id
is single-lane by construction, so "5 ops, 64 MiB, 1 lane" (Standard Notes'
004 parameters) is exactly `crypto_pwhash`'s own defaults for those two
knobs - nothing to configure for the lane count. The salt is 16 bytes
(`crypto_pwhash_SALTBYTES`), generated once at account creation and fetched
from the server by email before the first derivation on a device (a salt is
not secret; what it defeats is a precomputed table shared across accounts). A
server asked for an unknown email's salt gets `crypto_generichash(email,
key=serverSalt)` back instead of an error, so the response shape does not
reveal which emails are registered.

`masterKey` lives in memory only - `sodium_mlock`ed for the process lifetime it
is needed, `sodium_memzero`d on sign-out or destruction - and is re-derived
from the password whenever the engine has nothing usable cached for the
session. "Nothing usable" means: no `wrapKey` derived this session, no keys in
the OS keyring for this device (next section), and no itemsKey already
unwrapped (which can also come from a lockbox - see enrolment). This is the
one place the app calls back into the UI to ask for a password
(`SyncEngine::PasswordPrompt`, `start()`'s callback parameter).

### Keys kept in the keyring

The password is asked once per device, not once per session. After the server
accepts a sign-in made with freshly derived keys, the engine writes those keys
(never the password, never `masterKey`) to the OS keyring through QtKeychain:

| | |
|---|---|
| service | `noteAppC-sync` (the same one the device identity key uses) |
| key | `keys/<server URL, no trailing slash>/<local device ID, hex>` |
| value | 97 bytes: a version byte `0x01`, then `loginKey`, `wrapKey`, `contentKey` (32 B each) |

The device ID is the one in this vault's local journal, so the entry is per
(server, vault on this device). `loginKey` and `wrapKey` are one-way
derivatives of `masterKey` but are as good as the password for this account's
sync (the first signs in, the second unwraps the itemsKeys), so the keyring is
the trust boundary, exactly as for the device identity key.

At startup the engine reads the entry before asking anything, and only once per
start. If it is missing or malformed, the password prompt is asked (with no
password to give, the status is `NeedsPassword` - "Sync: not signed in" - and
nothing touches the network). If the server answers a sign-in with the stored
`loginKey` and refuses it (the password was changed on another device), the
entry is deleted, the salt is fetched again, and the password is asked. An
unreachable server is not a refusal: the entry stays and the cycle just tries
again later. The entry is rewritten after a password change made here.

"Sync: Forget this device's keys" (`sync:forget-keys`, `SyncEngine::forgetStoredKeys()`)
deletes the entry and stops the engine; the next sign-in asks the password.
Turning sync off leaves the entry alone (switching it back on should not cost a
password), and so does changing the server or account in settings - the old
entry then names a server nobody syncs to; "Forget" before changing it
removes it. Every keyring call runs on the sync worker thread under the same
15 s watchdog as the device identity key; the UI thread never waits on the
keyring. Without Qt6Keychain sync is not built at all (see the root
`CMakeLists.txt`), so there is no such path to configure.

### Subkeys

`crypto_kdf_derive_from_key(subkey, 32, subkey_id, context, masterKey)`,
`masterKey` as the 32-byte KDF key, three 8-byte ASCII contexts (the `contentKey`
one only seeds the per-device key described under "Content addresses"):

| context | subkey_id | produces |
|---|---|---|
| `"login001"` | 0 | `loginKey` |
| `"wrapkey1"` | 0 | `wrapKey` |
| `"content1"` | 0 | `contentKey` |

BLAKE2b-keyed, one-way: knowing `loginKey` does not yield `masterKey`, which
is the property "the server stores only an Argon2id hash of a login key the
client derives separately from the master key - never the password or
anything that yields the master key" needs.

### itemsKey and noteKey

`itemsKey`: `randombytes_buf(32)`. One active key per vault normally; a
password change does not rotate it (see below), only an explicit "rotate
keys" action would, which is not built yet (nothing consumes it - YAGNI until
there is a revocation story to drive it).

`noteKey`: `randombytes_buf(32)`, one per entity (note, attachment or folder),
generated the first time that entity is pushed. Its wrapped form travels
alongside the entity's journal record (below) so any device holding the
current `itemsKey` can unwrap it. The extra tier over "just use itemsKey
everywhere" buys two things: rotating a compromised single note's key never
touches the others, and an itemsKey compromise is the only thing that exposes
more than one note at once - matching the spec's three tiers literally rather
than collapsing it to two.

### Wrapping (AEAD)

Every "wrap" and every journal payload uses XChaCha20-Poly1305
(`crypto_aead_xchacha20poly1305_ietf_*`), 192-bit random nonces
(`crypto_aead_xchacha20poly1305_ietf_NPUBBYTES` = 24, `randombytes_buf`), 32-byte
keys, 16-byte tags. Every call binds associated data built from the record's
own identifiers and a one-byte protocol version, so the server cannot swap
ciphertext between records, replay a record under a different identity, or
downgrade the protocol without the tag failing to verify:

```
wrap(itemsKey,  plaintext=noteKey,        AD = version | "IK" | entityID | itemsKeyID)
wrap(noteKey,   plaintext=EntityRecord,   AD = version | "EN" | entityID | deviceID | recordID | itemsKeyID)
wrap(wrapKey,   plaintext=itemsKey,       AD = version | "WK" | itemsKeyID | wrapGeneration)
```

`SyncCrypto::seal`/`open` (`libs/ext/sync/SyncCrypto.h`) take the key, the
plaintext and the AD components already concatenated by the caller (each
caller owns building its own AD, so it cannot forget a field a sibling
includes) and return/consume `nonce || ciphertext || tag` as one byte string.
Every `open` failure returns false and the caller drops the record; nothing
about *why* verification failed is exposed (no "tag ok, but AD mismatch" vs.
"decryption failed" distinction), which is what keeps a padding-oracle-style
probe from learning anything.

### Content addresses

`crypto_generichash` (BLAKE2b), keyed with `contentKey`, 32-byte output, over
`entityID || ` the **padded, still-plaintext** blob before encryption (the
entity's ID is in the preimage because the blob is sealed under that entity's
`noteKey`: with the bytes alone, two entities holding equal bytes shared one
blob sealed under one `noteKey`, and the other could not open it - fixed in
round 9; receivers read the address from the record, so blobs written under
the old rule still open). Keying is what makes
"never raw SHA of plaintext" true: an attacker who does not have `contentKey`
cannot precompute the address of a guessed plaintext ("does this vault contain
the string X") the way they could against an unkeyed hash. With the entity ID in
the preimage there is no cross-entity dedup: two attachments with identical
bytes are two blobs, so the server no longer learns that they match (the same
bytes pushed again by the same entity do address the same blob, so a retried
`PUT` is a no-op). Note bodies too big to inline use the same rule.

`contentKey` is a **per-device local secret**, not something the devices share:
it keys this device's own change detection (`localHash`, `syncedHash`) and the
blob addresses it writes, and nothing a second device must reproduce - a
blob's address travels in the record, and the hashes are only ever compared
with this device's own. It is stored in the journal's `meta` table
(`"contentKey"`), seeded once from the key derived at first sign-in, and from
then on read back from there instead of derived: **a password change leaves it
alone**, so the hashes already on file stay valid and the next scan does not
see every file as changed. It sits in the journal beside the itemsKeys, which
are already held in the clear there, so it exposes nothing new.

### Padding

`sodium_pad`/`sodium_unpad` round every plaintext blob up to the next entry in
a fixed bucket table before it is encrypted, so ciphertext length reveals only
the bucket, not the byte count:

```
1 KiB, 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB, ... (x4 to a 512 MiB cap)
```

`SyncCrypto::pad(bytes)` picks the smallest bucket `>= bytes.size() + 1` (the
`+1` matches `sodium_pad`'s own requirement that padding be unambiguous even
when the input already lands on a boundary) and pads to it;
`SyncCrypto::unpad(padded, blocksize)` reverses it. Journal payloads (small,
metadata-sized) use the two smallest buckets; blobs use the whole table.

### secretstream for blobs

Everything in the blob store - attachments, and note bodies too big to
inline in a journal payload (see "Record and blob formats") - goes through
`crypto_secretstream_xchacha20poly1305` in `kStreamChunk` = 64 KiB chunks
rather than one AEAD call, one code path regardless of size: a small blob is
simply a header plus one chunk, a large one never has to be held twice in
memory (plaintext and ciphertext) to encrypt it. `init_push`/`push` on the way
up, `init_pull`/`pull` on the way down; the stream's own header carries its
random nonce; each `push` call's `ad` parameter is the same AD a plain AEAD
wrap would use for that blob (`version | "BL" | contentAddress`), and the
final chunk is tagged `TAG_FINAL` so a truncated download is detected (a
`pull` loop that never sees `TAG_FINAL` is a truncated or tampered stream and
the partial plaintext is discarded, never handed to the caller). Journal
payloads (always small, metadata-sized) stay plain AEAD - see "Wrapping
(AEAD)" - only the blob store uses secretstream.

## Record and blob formats

### EntityRecord (the plaintext structure, once a journal payload is opened)

```
entityType   : note | attachment
path         : vault-relative path, UTF-8, LF-normalised same as Vault::Note
op           : upsert | delete
mtime        : int64, client clock, advisory only (never used for ordering)
body         : inline UTF-8 text, when entityType == note and body fits
               under 16 KiB after padding - most notes, so most syncs never
               touch the blob store at all
blobAddress  : 32 B, present instead of body for attachments and oversized
               notes
parent       : 32 B, optional: the digest of the state the writer built this
               record on (absent for a new entity, and from a client that
               predates parents)
hash         : 32 B, optional: the digest of this record's own content (note:
               the UTF-8 of the LF-normalised text; attachment: the raw
               bytes; absent on a delete)
```

A *digest* is `SyncCrypto::digest`: BLAKE2b-256 with no key. Unkeyed on
purpose - `contentKey` is per device (see "Content addresses"), so a keyed hash
sealed into a record could only be checked by the device that wrote it. Both fields live inside the AEAD payload, so
the server learns nothing from them, and a client that does not know them
ignores them; a record without them is a legacy record and is applied by this
device's own state alone (see "Conflict rules").

Folders are not their own entity: an empty folder carries nothing worth
syncing, and a folder rename already surfaces as every note under it getting
a new `path` in its own next record (`Vault::renamePath` only ever moves one
path at a time, so that is also all the local scanner can observe in one
pass). Renames are detected by content: a path with no known entity is
matched against a *missing* entity (one whose last-known path no longer
exists) with the same content hash, and treated as that entity moved rather
than a new one created - ambiguous only when two files have byte-identical
content and both move in the same scan, in which case one is picked
arbitrarily (`SyncScanner::scan`, `libs/ext/sync/SyncScanner.cpp`); documented
rather than solved, since Obsidian's own rename tracking has the same
limitation. A note renamed *and* edited before the next scan has no exact
match; it is then compared line by line (Dice coefficient over the non-blank
lines) with the notes that vanished in the same scan, against each one's merge
base, and taken as that note moved when the best match scores at least 0.5 and
clearly beats the second best. Only when the new notes times the vanished notes
come to at most 64 (a folder of edited notes moved at once falls back to a
delete plus new notes), and only on a scan that has both, so a scan with
nothing new reads no file. A wrong match gives the new note the old one's
identity; no text is lost.

The false positive that does happen: notes made from one template share most
of their lines, so deleting one and creating another from the same template
before the next scan scores well above 0.5 and reads as a rename. The new note
then carries the deleted note's identity (its history and merge base), so
other devices see an edit and a rename of the old note instead of a delete and
a new note; the new text still arrives everywhere. Pinned by `SyncScannerTest.ANewNoteFromTheSameTemplateAsADeletedOneIsTakenForItsRename`.

There is no separate "rename" or "move" op: a rename is an `upsert` whose
`path` differs from the last one recorded for the same `entityID`, the same
way Trilium's own entity-change log has no rename op either (a note's title
and a branch's parent are just fields that changed). This also means a folder
rename is one record per note under it that the scanner still considers
"moved" - `libs/vault/Vault.cpp`'s own `renamePath` only ever touches one path
at a time, so this matches what the local scanner can actually observe.

### Journal record (what the server stores and orders, one row per push)

```
seq          : server-assigned, strictly increasing per vault - this is what
               pull resumes from and what LWW orders by (never the client's
               mtime: a client clock cannot be trusted to break ties, and
               server seq is assigned atomically at insert, so two devices
               racing to push are ordered exactly once)
entityID     : 16 B random, generated client-side the first time an entity is
               pushed, stable across renames
recordID     : 16 B random, generated client-side per push attempt
deviceID     : 16 B
itemsKeyID   : 16 B, selects which itemsKey unwraps wrappedNoteKey
wrappedNoteKey : AEAD(itemsKey, noteKey)          -- "IK" AD, above
payload      : AEAD(noteKey, EntityRecord)        -- "EN" AD, above
size         : the padded ciphertext length actually stored (bucket size)
```

`UNIQUE(deviceID, recordID)` server-side is what makes a push idempotent: a
client that pushed a batch, lost the connection before seeing the response,
and retries gets back the same seqs the first attempt would have been
assigned, not a duplicate row.

### Blob (attachments and oversized note bodies)

Stored server-side at `<data>/blobs/<address[0:2]>/<address>`, `PUT` is
idempotent (content-addressed - writing the same bytes twice is a no-op, and
one entity's retried push is a no-op; two entities never share a blob, per
"Content addresses" above). The file on disk is exactly `secretstream`'s
output: its header, then one or more `push` chunks, the last tagged
`TAG_FINAL`. The blob's own `contentAddress` never appears inside the
plaintext it addresses (it is computed over the plaintext, so including it
would be circular) - it only ever appears as a field of the `EntityRecord`
that references it, itself encrypted.

## The journal protocol

### Push

1. Client batches its pending local `EntityRecord`s (from `SyncJournal`'s
   outbox), builds `wrappedNoteKey` + `payload` for each, and `POST`s the
   batch to `/v1/vaults/<id>/journal`.
2. Server, inside one transaction per batch: for each record, if
   `(deviceID, recordID)` already exists, returns its existing `seq`
   (idempotent replay); otherwise inserts it and assigns the next `seq`.
3. Client marks each pushed record's local outbox row done and advances
   nothing else - a push never changes what pull resumes from.

### Pull

1. Client asks for `GET /v1/vaults/<id>/journal?since=<lastPulledSeq>&limit=500`.
2. Server returns rows in `seq` order, `seq <= since` never repeated.
3. Client applies each in order (see "Conflict rules"), and only after a
   record is durably applied (written to the vault or merged) does it advance
   `lastPulledSeq` to that record's `seq`, in the same local transaction as
   the write to `SyncJournal`'s own SQLite database. A crash between "wrote
   the note" and "advanced lastPulledSeq" re-pulls and re-applies the same
   record; applying an `EntityRecord` is naturally idempotent (it sets the
   note to a specific state, it does not "add" anything), so this is safe
   without extra bookkeeping.
4. `limit=500` plus resumption from `lastPulledSeq` is what makes a large
   pull safe to interrupt - there is no "start the pull over" path.

### Paths from records are confined to the vault

A record is authenticated, but a path inside it is still input from another
device, and a device holding the vault keys can be compromised or buggy (it
must not be able to write `~/.bashrc`). So before any file is read, written,
moved or trashed, `Vault::isConfinedPath` checks the record's path, and the
path the entity already has here: relative; no empty, `.`, `..` or dot-prefixed
segment (so no `.obsidian/plugins/...`); no backslash or NUL; and the nearest
existing ancestor really inside the vault, so a symlinked folder that points
out of it is no way out. The entity's own file may not be a symlink either, so
a pulled edit is never written through one. (A link at a *new* target is the
occupied-path case in "Conflict rules": kept as a copy beside it.) The check is
made once, so every write that follows (the record's text, a merge result, a
conflict copy) goes through `Vault::writeBytesNoFollow` / `writeNoteNoFollow`,
which walk the path with `openat(O_NOFOLLOW)`: a link that appeared since, or a
folder that is a link to somewhere else inside the vault, fails the write
instead of being followed. A failed write drops the record like a refused one -
named, nothing recorded as applied, and never after the text it would replace
was lost (a conflict copy that cannot be written stops the replacement too).
A delete retried later (a parked one, below) repeats the check on the entity's
own path first, since the file may be a link by then. A refused
record is dropped, like one that cannot be decrypted - it could never succeed,
so it must not hold the cursor back - and its path is named in the status
message for the rest of the session. Outgoing, the scanner skips symlinks, so
a link's target (maybe outside the vault) is never pushed as a note, and a
local change whose path fails the same check (a backslash in the name, a dot
folder) is not pushed either: it is named in the status message ("refused:
paths outside the vault or with odd names"), and a delete of such a file just
forgets the entity.

### Resumption

`lastPulledSeq` and the outbox both live in `SyncJournal`'s SQLite database
under `$XDG_DATA_HOME/noteAppC/sync/<vault digest>/journal.db` (see "What the
client keeps" below) - never in the vault, per the non-negotiable rule.
Losing that database is not catastrophic: `lastPulledSeq` resets to 0 and a
full pull re-applies everything (idempotent, if slower), and the outbox
rebuilds itself from a full rescan.

## Conflict rules

Three-way merge needs a base: the text both sides last agreed on. `SyncJournal`
keeps the *current* base per note-entity under
`$XDG_DATA_HOME/noteAppC/sync/<vault digest>/base/<entityID>` - a plain UTF-8
copy, written (through `QSaveFile`, so a crash or a failed write leaves the
previous base, never an empty one) only when a pull is applied or a push is
confirmed. It is local-only and never syncs. The base is always **the sender's
text**, never a merge result: a merge result is a text no server record holds,
and a later record built on the sender's text would then be merged against the
wrong ancestor.

### Which state a record builds on

Every record names the state its writer built it on (`parent`, a digest) and
its own (`hash`). A device keeps, per entity, the digest of its last confirmed
sync point (`baseDigest`, what `base/` holds), the digest of the last state it
wrote or applied (`headDigest`, the `parent` of its next record), and the last
32 states it wrote or applied (`states` table: digest, text, local path, server
seq). On pull, for a note:

- **in line** - the record's `parent` is empty (a new entity, or a legacy
  writer), or this device has no `baseDigest`, or `parent == baseDigest`: the
  sender saw what this device last confirmed. Local unchanged (the file still
  hashes to what was last confirmed) is a fast-forward: write the record's text,
  in this file's CRLF/BOM. A note hashes as its decoded text everywhere (scanner,
  pull, delete), so a re-save that only changes the line endings or the BOM is
  no edit: it pushes nothing and does not stop a remote delete.
  Local changed is a three-way merge against `base/`.
  A journal from before digests has no `baseDigest`; the first record that meets
  an unchanged note takes the digest of `base/` (the confirmed text) instead, so
  a record built on another state is not read as in line and fast-forwarded over
  a text its sender never saw.
- **not in line** - the sender built on a state this device has since moved
  past (two devices wrote concurrently, and a third record or this device's own
  confirmation came in between): the state is looked up in `states` and the
  merge is `(that state's text, local, remote)`. This is what a fold over
  `seq` order alone cannot do - the fold merged against whatever the previous
  record left behind, and applied a record written without seeing this
  device's edit as a fast-forward over it. A state older than the 32 kept (a
  device offline for more than 32 saves of one note) is not found and takes the
  conflict rule below, never a loss; an attachment has no text to merge and
  always does.
- identical content on disk and in the record: nothing to write; only the path
  (below) and the confirmation.

The merge is a line-based diff3 (`SyncMerge::merge`,
`libs/ext/sync/SyncMerge.cpp`): the LCS-based line matcher already in
`libs/ui/RecoveryDialog.cpp` (`lineDiff`) extended to three inputs the usual
way - diff `base` against `local` and `base` against `remote`, then walk both
edit scripts together, taking a hunk from whichever side touched it and
flagging a **true conflict** only where both sides edited the same base lines
differently (edits on neighbouring lines count as the same region).
`libs/ui` is out of this lane's file ownership, so the LCS
matcher is a small, separate copy in `libs/ext/sync/SyncMerge.cpp` rather than
a shared extraction - worth moving to somewhere both can include if a third
user of line-diffing turns up, not before (YAGNI).

Once a record is applied, `base/` holds its text, `baseDigest` and
`headDigest` its digest, and `syncedHash` its keyed hash; a device's own pushed
records are confirmed the same way when they come back (the self-echo), from the
state the device wrote - never from whatever the vault file holds by then. An
echo whose `parent` is no longer the `baseDigest` is skipped: a remote record
was applied in between and its merge result carries that content. Two
exceptions keep the base from falling behind a file that already holds the
content: the echo still moves it when the file's own hash (what the scanner
last saw) is the echoed record's, and when the record's row was pruned from
`states` (a note scanned more than 32 times with no push in between, say) and
its `parent` is the base, the record itself (inline body, or its blob) supplies
the text the base and the synced hash are moved to.

- **Clean merge**: write the merged text (atomic, preserves this file's
  CRLF/BOM) and confirm the sender's record as above. When the merged text
  holds something the sender's did not, **it is announced**: one record,
  `parent` the sender's text, so every device that has not seen it reaches the
  same text from one source instead of re-deriving it. Not when the merged text
  is the sender's (equal content, a fast-forward: nothing to say), and not when
  the merge left this device's own record's text as it was: that record is
  already on its way, and a second record for the same text could revive a note
  deleted in between. The other way round is accepted: a merge that lands on
  a text another device wrote (not this device's own record) is still
  announced, though that device reaches the same text by its own merge, so the
  record is redundant for it and, if it deleted the note meanwhile, revives it
  (an edit that added nothing beats the delete). Rare, and nothing is lost.
  The announce is **coalesced**: a pull marks the entities
  whose merge holds something new and sends one record per entity when the pull
  ends (on success or on the error that stops it), whatever number of records
  of the batch merged; its `parent` is the last state the batch applied. An
  announce that cannot be made (a merge result of 16 KiB or more is a blob, and
  the upload failed) clears the entity's stat and hash, so the next scan reads
  the file as changed and pushes it; otherwise the scanner would take the file
  for already synced and the other devices would never get the merge. So a
  device sends at most one merge record per note per pull, and the exchange
  ends: three devices editing three different lines of one note converge in
  7 records (3 edits and 4 merge results, one device merging in two pulls).
- **True conflict** (an unreconcilable merge, a state that is not found, an
  attachment): the server's order decides, because both devices see it. This
  device's text is the one it last wrote or applied; its server seq is in
  `states` (0 while not yet pushed or acknowledged, which counts as newest). If
  this device's text has the higher seq (or none), the file stays exactly as it
  is and nothing is written (the base moves to the incoming text as for any
  record, but the state its next record builds on stays its own text, which is
  what the other devices that took it hold): the other device, whose text is the
  older, makes the copy. If the record's seq is higher, this device is the **loser**: it
  writes a copy of **its own text** beside the file,
  `<name> (sync conflict, <device>, <timestamp>).md`, then replaces the file
  with the incoming text. The loser copies, not the winner, because a conflict
  may be seen by one device only (the other merged cleanly, having a state this
  device has pruned): a rule that has the winner copy the loser's text loses
  that text. The copy is an ordinary new note and reaches the other device
  through the next push. Either way a status signal reports the conflict. So
  there is exactly one copy for two devices. A loser whose text on disk is
  another device's (a state it applied, not wrote) makes no copy: that text is
  the other device's to copy if it loses there, so three devices changing one
  line three ways end with exactly two copies and the winner's text in the
  note, on every device. If the text on disk has no row in `states` (saved
  between the scan and the pull, or pushed before this journal had rows) this
  device is the winner by the rule above and announces that text as a record of
  its own, so it is not left out - and, as the one exception, it also writes a
  copy of the **incoming** text: the other device may have won over a text it
  never saw, and the loser who would copy it is this device, which sees no
  conflict where the winner is elsewhere.
  Folding a copy in by hand is an ordinary local edit.
- **The device whose history moved on takes the conflict.** A record built on
  a state older than the 32 this device keeps cannot be merged here, so this
  device sees a conflict where a device that still has the state merges
  cleanly. If its text is the older one it is the loser, makes the copy of its
  text and takes the sender's, and the clean merge from the other device then
  reaches it as an ordinary record (announced as above) - nothing is lost, but
  the device that kept up is not the one that pays for it.
- **Devices from before `parent` and `hash`** (their records carry neither)
  are applied by what this device holds alone, as in "in line" above, and
  their conflicts are decided by the same seq rule. A conflict between an old
  client and a new one is the one case that is not guaranteed to converge: the
  old rule had the winner copy the loser's text, the new one has the loser copy
  its own, and neither side knows what the other does. This was so before and
  is not fixed here; update every device.
- **A different entity on the same path**: a pulled note or attachment this
  device has no entity for, whose path already holds a file (or anything else) (a parked
  delete's file the person has edited since, or the same path created on two
  devices), never overwrites it. Different content is written as a conflict
  copy and the local file stays; identical content writes nothing. No entity
  is made for the incoming one - two entities on a path would make the scan
  read one of them as deleted - so a later edit to it from the other device
  conflicts again until one side renames the note.
- **A rename onto an occupied path**: a pulled rename or move whose target
  already holds a file the renamed note does not own (another note of this
  device, or a stray file) is not applied - the move would overwrite that
  file. The note stays at its own path and takes the incoming content there,
  so nothing is lost and no copy is made; later edits keep landing on it, and
  the rename goes through by itself on the first record that finds the target
  free. It counts as a conflict unless the target already holds the incoming
  content. "Occupied" means anything at the path: a directory or a symbolic
  link (even a dangling one) counts too, so a pulled note is never written
  through a link to wherever it points, and a new note meeting one is kept
  as a conflict copy beside it.
- **A rename of a note this device edited**: the file moves to the new path
  first and the merge happens there, so the file and the entity agree. This
  device's edit may already have been pushed with the old path, but a record
  that names the old path does not move other devices back (see "Which path
  wins"). A move that cannot be done leaves the note at its own path, like an
  occupied target.
- **Which path wins**: the states also record the local path each was applied
  at, and a record whose path differs from this device's is judged by what it
  is.
  - A **pure rename** (its content is its parent's) is settled by the server's
    order: this device's own rename stands only if it was pushed after the
    record (or not pushed yet, which counts as newest); otherwise the record's
    path is applied. So when two devices rename one note concurrently, every
    device - the two renamers and any third - ends on the one the server saw
    last, and no device's own-ness decides.
  - Any **other record** (an edit, possibly with a rename in it) carries the
    path its sender had. When that is a path this device had at the state the
    record builds on, the sender did not rename, and this device's current path
    stands: a rename made here, or a rename it held back above. A third device
    that applied a rename keeps it against a crossing edit still naming the old
    path, because it recorded the old path at that state too. A path the device
    never had there is a rename and is applied as above.
  - Known edge: renaming a note back to a name it had before *and* editing it,
    before any sync between, is one record whose path matches an older row, so
    it is read as "no rename" and the note stays at the other name there. A
    rename alone (the rename back with no edit) is unaffected.

### Delete-vs-edit

**An edit beats a delete, in both directions.** A delete record carries the
state its writer last saw, like any record. If that is not the state this device
last confirmed, the deleter had not seen a later edit: the delete is ignored
whole, and the deleter receives that edit as an upsert in its next pull and
gets the file back. (Only when the deleter is the one behind - its state is
older than this device's base by server seq, or either is unknown. A deleter
that is ahead of a base that fell behind a skipped echo is judged by the local
edit rule below instead.) If the entity has a local edit since `base` (`base !=
local`), the delete is not applied either - the local file survives, the entity
is kept (not dropped and later pushed as an unrelated new note), and the note is
pushed again as that entity's next record, so other devices' delete does not
keep winning a race against an edit that only needed to arrive. Symmetrically, if the local scanner
finds a note gone (removed or trashed via `Vault::moveToTrash`) but a pull
brings in a remote edit newer than `base`, the edit is written back to the
vault path (undeleting it locally) rather than discarded. Losing content is
judged worse than an unwanted file reappearing - the reappearance is visible
and reversible (delete it again); a silently lost edit is not.

A delete only actually removes a note when both sides agree the last thing
that happened to it was the delete (`base == local`, i.e. nothing local
disagrees).
If moving the file to `.trash` then fails (a read-only or full disk), the
pull stops at that record: the entity stays, the cursor stays before it, the
status says which note, and the next cycle tries again. Dropping the entity
with the file still there would make the next scan push it back as a new
note and undo the delete on every device.

A failure that never goes away (a read-only file, a trash that never works)
must not hold every later record from every device behind it forever. After
three cycles of the same delete failing, the pull moves on past it and the
delete becomes a *pending delete*: the entity stays (so the scan still sees
the file as synced and never pushes it back as a new note), the entity's ID
is kept in the journal's `meta` (`pendingDeletes`, so a restart still retries
it), and every cycle retries the pending deletes before it pulls. The status
is `Idle` (a parked delete is not a failure, so the bar still reads "synced")
with the message "N deletion(s) from other devices could not be applied:
names; retried each sync" as its tooltip until they all go through. The
failure count is in memory only and restarts at each launch, so after a
restart a delete that was not yet parked blocks three more cycles. A later record for the
same note from any device (an edit, a rename) wins over the waiting delete
and cancels it; a local edit does too, as above.

A blob that cannot be fetched (the server answers with an error, or the
blob is gone) is handled the same way. A record for an attachment or an
oversized note body holds the pull - the status is `Error` with "attachment
`path` could not be fetched; retried next sync", the cursor stays before it -
for three cycles, so a passing network failure never loses it. After the third
the record is parked: dropped from the pull (later records apply), the path
kept for the session, and the status is `Idle` with "N attachment(s) from
other devices could not be read: paths". A blob that is there but cannot be
opened (sealed under a key this device lacks) is parked at once, retrying
cannot help. The count is in memory only, like the delete count; a restart
starts it over and forgets what was parked, so the record is not re-pulled
(the cursor has passed it) - the file reappears when its entity next changes
on any device.

## What the server stores and learns

Stores, all under `--data`: an `accounts` table (email, salt, `loginKey`'s
`crypto_pwhash_str`, nothing else identity-shaped), a `sessions` table
(bearer token hash, expiry), a `devices` table (deviceID, X25519 public key,
approved bool), a `journal` table per the row shape above, a `lockboxes`
table (`forDeviceID`, `itemsKeyID`, sealed blob), and content-addressed blob
files. No plaintext note text, no plaintext file or folder names (they live
inside `payload`), no tags (same), no plan of the vault's tree beyond "this
many entityIDs exist and got this many records" - the tree itself
(parent/child) is a field inside the encrypted `EntityRecord`, not a
server-visible structure.

What it **does** learn, honestly: how many entities a vault has and roughly
how often each changes (record counts and `seq` gaps per `entityID`); ciphertext
sizes rounded up to a padding bucket (so "a 3 KB note" is visible as "a note in
the 4 KiB-or-under bucket", never 3 072 bytes exactly); timing of when pushes
and pulls happen (sync is not batched to disguise activity patterns - out of
scope, see threat model); (only blobs written before round 9, which were addressed by their bytes
alone, show which attachments are byte-identical within one vault); account existence is only
mitigated, not eliminated, by the salt-lookup response shape (a sufficiently
persistent enumeration attempt - timing the salt endpoint, watching account
creation - is not defended against, it is out of scope for a first cut);
IP addresses of connecting devices (ordinary for any HTTP server, no attempt
made to hide it, e.g. no mixnet).

## Device enrolment with lockboxes

A new device needs the itemsKeys before it can read anything, and there are
two ways it can get them:

1. **From the password directly** - the default, and what any device takes
   whenever it is given the password: sign in (derive `loginKey`,
   authenticate), then also derive `wrapKey` from the same password and
   `GET /v1/vaults/<id>/itemskeys` to unwrap whatever the server hands back.
   This is **not** gated by device approval: authenticating already proves
   the caller derived the correct `loginKey`, which means it could derive
   `wrapKey` too, so withholding the wrapped itemsKeys here would not stop
   a device that has the password, only force it onto the lockbox path for
   no real security gain - approval is a property of *that* path, not this
   one (an earlier draft of this design gated both, which meant a second
   device with the correct password could not get in without a lockbox
   either, an unintended and pointless bit of friction caught by the first
   run of `tests/test_sync.cpp`'s two-device test).
2. **From a lockbox** - for a device that should skip the password re-entry
   on purpose (or, later, a collaborator who was never given the password at
   all): the new device generates an X25519 identity keypair
   (`crypto_box_keypair`), keeps the secret half in the OS keyring via
   QtKeychain, and after authenticating (still needs `loginKey`, so still
   needs the password once, to prove account ownership - a full stranger
   cannot just claim to be a new device), registers its public key as a
   *pending* device (`POST /v1/vaults/<id>/devices`, with `"lockbox": true`).
   Any device that already holds the itemsKeys in memory notices the pending
   device on its next sync pass and **asks a person** (`deviceAwaitingApproval`,
   a prompt in the window) showing the device's fingerprint - BLAKE2b of its
   public key, 80 bits, `3F2A-91C0-5B7E-0D44-A1C9` - which the new device
   shows too, in its waiting status. Only once approved there (remembered per
   vault on that device, never on the server) does it seal each active
   itemsKey to the new device's public key
   (`crypto_box_seal` - anonymous sender, only the recipient's key pair is
   involved, which is the right shape for "device I've never talked to
   before") and uploads the sealed blob (`POST /v1/vaults/<id>/lockboxes`) -
   which is also the signal the server uses to flip the pending device to
   approved, since an existing device sealing something for it is exactly
   what "vouched for" means here. The new device pulls its lockboxes and
   opens them (`crypto_box_seal_open`) with its own secret key. It now holds
   the itemsKeys without ever deriving `wrapKey`.

Why a person and not the server decides: the server is never trusted with
the keys, so it cannot be trusted to say which devices should get them. When
existing devices sealed for every device the server listed, a server could
list a key of its own and be handed the vault. With approval it has to get a
person to accept a code that no device of theirs shows. A device registered
without `"lockbox": true` (one that signs in with the password) is never
asked about; a server that lies about that flag can only hide a prompt.

Path 2 is the one the tests exercise, because it is the one that generalises:
a future revoked device simply stops receiving new lockboxes (existing
itemsKeys it already has stay readable for old content, which is correct -
revocation forward-secures new keys, it does not retroactively un-decrypt
history without rotating every itemsKey, which is a real limitation worth
being honest about, not solved here), and "later collaborators" is the exact
same call with a collaborator's public key instead of a device's, scoped to a
shared note's key instead of the whole vault's itemsKey - left for the CRDT
stage.

## Password change

1. New salt, new `masterKey'` (Argon2id over the new password), new
   `wrapKey'` and `loginKey'` (same KDF contexts as before).
   `contentKey` is not derived again (see "Content addresses"): it never
   follows the password.
2. Every itemsKey this device currently holds in plaintext (it must, to be
   useful) is re-wrapped with `wrapKey'` under a fresh nonce - **only the
   wrap changes**, the itemsKey bytes themselves do not, so every noteKey and
   every journal record already synced stays valid with no rewrite.
3. One authenticated request (authenticated with the *old* `loginKey`, since
   the old session is still live), `POST /v1/account/password`, carries the
   vault this device is managing, the new salt, the new `loginKey'` (server
   re-hashes it with `crypto_pwhash_str`, discarding the old hash), and that
   vault's re-wrapped itemsKeys, applied atomically.
4. A device that is offline during the change keeps working against its
   cached (plaintext-in-memory-only, for that session) itemsKeys - a
   restart makes it fall back to "nothing usable", so it just needs a fresh
   sign-in (old password fails, new one works) or a lockbox from a device
   that already switched, same as ordinary enrolment.

**Scoped to one vault.** The call rewraps the itemsKeys of the vault the
initiating `SyncEngine` is managing, not every vault the account might have -
`SyncEngine` is one engine per vault (docs/SYNC.md's own API is "start/stop
for a vault"), and it only ever holds the itemsKeys of the vault it started
for. An account with a second vault would need that vault's own engine to
run a follow-up rewrap against the new salt/loginKey; nothing today
triggers that automatically. Worth fixing once multi-vault accounts are
actually exercised - not before.

## What the client keeps (`$XDG_DATA_HOME/noteAppC/sync/<vault digest>/`)

Mirrors `libs/ext/history`'s own rule (never a vault dotfile, never inside the
vault): `<vault digest>` is `NoteHistory::repositoryPathFor`'s own hashing
scheme, reused so the two modules do not disagree about which directory names
a vault.

```
journal.db      SQLite: outbox, lastPulledSeq, entity table (entityID <->
                current path, itemsKeyID, noteKey wrapped for local storage
                by a key derived from the OS keyring's device secret - see
                below; baseDigest and headDigest), device row, known
                itemsKeys (wrapped), and the states table (the last 32
                states of each entity this device wrote or applied: digest,
                text, local path, own or applied, server seq)
base/<entityID> last-synced plaintext body, for three-way merge
```

The states table is what lets a record be merged against the state its writer
saw. It is bounded (32 rows per entity, the oldest dropped as new ones are
added), so a note saved a thousand times costs 32 note-sizes, not a thousand.
A journal from before parents has none of the new columns or rows; they are
added when it is opened and fill in as records are applied, and a record or
echo that carries no digest is handled the old way.

Each entity's row caches its `wrappedNoteKey` exactly as it travels on the
wire - still AEAD ciphertext under `itemsKey`, so storing it here adds no
exposure beyond what the server already holds; unwrapping it still needs an
`itemsKey`, held in the clear only in the `items_keys` table (see below),
protected by nothing beyond ordinary disk encryption, the same tier the vault
's own plaintext notes already sit at on this device. A device-local
re-wrapping layer on top was considered and dropped: it would protect a
narrower threat (this database read without the OS keyring, while the vault
right next to it stays fully readable) at the cost of a second key-management
path with its own bugs - not worth it until something other than "disk
encryption" is the assumption.

## What is left for the CRDT stage

- Note **bodies** are synced file-level (three-way merge, conflict copy on a
  true clash) - not merged character-by-character. A CRDT per note body
  replaces `SyncMerge` for the body field specifically; the journal/tree
  layer above (entity records, LWW by server seq, the key hierarchy) does not
  change, because Trilium's model this borrows is already metadata/tree-only
  by design - the plan called for CRDT bodies from the start, this is not a
  retrofit.
- Real-time presence, cursors, and sub-second propagation are not attempted;
  sync is timer/batch-driven off the file watcher, not a live channel.
- Collaborator lockboxes (sharing a single note's key with another *account*,
  not another device of the same account) reuse the enrolment mechanism above
  but are not wired up - there is no UI or server concept of a second account
  touching one vault yet.
- itemsKey rotation (as opposed to wrap-only rotation on password change) has
  no caller yet - nothing revokes a device today, so nothing needs it. The
  format supports multiple itemsKeys already (`itemsKeyID` on every record),
  so adding rotation later does not touch the wire format.

## The test suite never touches the OS keyring

The device identity key and the stored keys normally go to the OS keyring
through QtKeychain. `tests/test_sync.cpp` and `tests/test_window.cpp` set an
in-memory `SyncEngine::SecretStore` in their `main()` instead, for two reasons. A test run must not leave device keys in
the user's real keyring. And a secret-service backend that never answers (a
locked keyring, a prompt nobody sees) held each engine for the full 15 s of
`runKeychainJob`'s watchdog: that was the whole of the "per-process
slowdown" once noted here, 30 s for most two-device tests, and the suite
now runs in about five seconds.

`QStandardPaths::setTestModeEnabled(true)` stays out of the suite as well:
it overrides the per-test `XDG_DATA_HOME`, so every test's `SyncJournal`
wrote into one shared, never-cleaned `~/.qttest/share/noteAppC/sync/`.
