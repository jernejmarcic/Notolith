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
`masterKey` as the 32-byte KDF key, three 8-byte ASCII contexts:

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
the **padded, still-plaintext** blob before encryption. Keying is what makes
"never raw SHA of plaintext" true: an attacker who does not have `contentKey`
cannot precompute the address of a guessed plaintext ("does this vault contain
the string X") the way they could against an unkeyed hash. It does **not**
hide equality *within* one vault once you have `contentKey` - a
content-addressed blob store is inherently a dedup store, so two attachments
with identical bytes address the same blob and the server can see that they
match, the same way it could with an unkeyed hash if it already had a copy to
compare against. That is an honest, documented leak, not an oversight; see
"What the server learns". Note bodies are **not** content-addressed for dedup
(see "Record and blob formats") specifically to avoid that leak on the data
users care most about - only attachments use the shared blob store, where the
storage win is worth the caveat.

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
```

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
limitation.

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
this is the one place two entities' ciphertext can visibly coincide, per
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

### Resumption

`lastPulledSeq` and the outbox both live in `SyncJournal`'s SQLite database
under `$XDG_DATA_HOME/noteAppC/sync/<vault digest>/journal.db` (see "What the
client keeps" below) - never in the vault, per the non-negotiable rule.
Losing that database is not catastrophic: `lastPulledSeq` resets to 0 and a
full pull re-applies everything (idempotent, if slower), and the outbox
rebuilds itself from a full rescan.

## Conflict rules

Three-way merge needs a base: the last plaintext body both sides agreed on.
`SyncJournal` keeps that base per note-entity under
`$XDG_DATA_HOME/noteAppC/sync/<vault digest>/base/<entityID>` - a plain UTF-8
copy, written (through `QSaveFile`, so a crash or a failed write leaves the
previous base, never an empty one) only when a push or a clean pull succeeds,
never touched by anything else. It is local-only and never syncs.

On pull, for a note entity, compare three hashes: `base` (what
`SyncJournal` has on file), `local` (what's on disk right now), `remote`
(what just arrived):

| base = local? | base = remote? | outcome |
|---|---|---|
| yes | yes | nothing changed either side - no-op |
| yes | no | fast-forward: write remote, update base |
| no | yes | local-only change, nothing to do (it will be pushed) |
| no | no | **both changed since base - three-way merge** |

The merge is a line-based diff3 (`SyncMerge::merge`,
`libs/ext/sync/SyncMerge.cpp`): the LCS-based line matcher already in
`libs/ui/RecoveryDialog.cpp` (`lineDiff`) extended to three inputs the usual
way - diff `base` against `local` and `base` against `remote`, then walk both
edit scripts together, taking a hunk from whichever side touched it and
flagging a **true conflict** only where both sides edited the same base lines
differently. `libs/ui` is out of this lane's file ownership, so the LCS
matcher is a small, separate copy in `libs/ext/sync/SyncMerge.cpp` rather than
a shared extraction - worth moving to somewhere both can include if a third
user of line-diffing turns up, not before (YAGNI).

- **Clean merge** (no overlapping hunks): write the merged text through
  `Vault::write` (atomic, preserves CRLF/BOM - the same path any other
  vault-write uses) and update the base to it. The merge result is **not**
  itself pushed as a new record - it does not need to be. The merge is a
  pure function of `(base, local, remote)`, so any other device that pulls
  the same two raw edits (its own, and the one that was just applied here)
  recomputes the identical text independently; pushing it too would only
  be extra traffic and a theoretical ping-pong if two devices both decided
  to "helpfully" re-announce a merge they just computed. Applying pulled
  records one at a time, in `seq` order, each against whatever `base` the
  *previous* record in the batch left behind, is what makes this converge
  for more than two devices too - it is a fold, not a pairwise special
  case.
- **True conflict**: the local file is left exactly as it is (local wins in
  place - never silently discard what's on screen), the remote version is
  written next to it as `<name> (sync conflict, <device>, <timestamp>).md`
  through the same atomic write path, and a status signal reports the
  conflict so the UI can surface it. The base is updated to match what is
  now on disk, i.e. the *local* text, not the remote one just declined -
  so the next cycle sees no change at all and stays quiet rather than
  re-flagging the same conflict every cycle until the user acts. Folding
  the conflict copy in by hand is then just an ordinary local edit against
  that base, pushed and reconciled like any other.

### Delete-vs-edit

**An edit beats a delete, in both directions.** If a remote record says
`delete` but the entity has a local edit since `base` (`base != local`), the
delete is not applied - the local file survives, and it is re-pushed as a
fresh `upsert` (so other devices' delete does not keep winning a race against
an edit that only needed to arrive). Symmetrically, if the local scanner
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
scope, see threat model); which attachments are byte-identical within one
vault (the content-address dedup caveat above); account existence is only
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
                below), device row, known itemsKeys (wrapped)
base/<entityID> last-synced plaintext body, for three-way merge
```

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
