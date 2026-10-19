# Sharing a note (in progress)

Live co-editing of one note between people, peer to peer. This page covers the
parts that exist: the text CRDT, the link layer, the engine and the session
that ties a note's editors to it. The dialogs (invitation, who has access), the
tab marker and the status badge come in a later lane (the plan is in the
project TODO).

## CrdtDoc

`libs/ext/share/CrdtDoc.h` is the interface the rest of sharing will use, and
the only thing it will see. A `CrdtDoc` holds the text of one note such that
replicas that have seen the same updates hold the same text, in whatever order
the updates arrived:

- `localEdit(at, removed, inserted)` applies a typed edit and returns the
  update to broadcast;
- `applyUpdate(update)` merges a remote one and returns the change as
  `CrdtEdit {at, removed, inserted}` records, UTF-16 offsets, each applied to
  the text the previous one left, so they feed `MarkdownEditor::applyExternalEdit`
  directly;
- `stateVector()` / `diffSince()` are how two peers that were apart find what
  the other lacks; `encodeState()` / `loadState()` are the on-disk form.

Updates are opaque bytes. Nothing about the engine behind the interface appears
in it, but every replica of a note must use the same backend: a Yrs update (Yjs
v1 encoding) and a Native one (its own format, below) do not read each other.

## Backends

Two, behind the same interface and the same test suite
(`tests/test_share_crdt.cpp`, one parameterised suite over `CrdtDoc::Backend`;
a backend the build lacks skips):

- **Native** - plain C++ on Qt6::Core, `CrdtDocNative.cpp`. Always built, on
  every platform; no Rust, no cargo.
- **Yrs** - the Rust [yrs](https://github.com/y-crdt/y-crdt) library, built by
  cargo from `libs/ext/share/yrs-ffi` into a static library and wrapped by
  `CrdtDocYrs.cpp`. Built when cargo is found and `NOTEAPPC_WITH_SHARE_YRS` is
  on (the default); otherwise it is silently left out.

Which one a note uses is the sharing layer's choice, not this library's. The
numbers that tell them apart are in [PERFORMANCE.md](PERFORMANCE.md#note-sharing-two-text-crdt-backends).

### Native: how it works

A run-length RGA with Lamport timestamps. The text is a sequence of items, one
per UTF-16 unit, each named `(client, clock)` (clocks run 0, 1, 2... per
client, so a replica's state is one number per client), each remembering the
item it was typed after (its origin) and a Lamport timestamp. A new item goes
directly after its origin, skipping every item that follows with a greater
`(timestamp, client)` key and stopping before the first smaller one. Every
replica that has the same items ends with the same order, whatever order they
arrived in. Items are stored as runs (item k of a run has clock, timestamp and
origin one more than item k-1), so text typed or pasted in one go is one run.
Deleting marks items as tombstones.

Chosen over YATA (what Yjs uses) and Fugue because the whole placement rule is
four lines with a textbook convergence proof, and run-length compression and
position lookup stay simple on a flat sequence. The invariants are written at
the top of `CrdtDocNative.cpp`.

- **Wire format.** `'N' 0x01`, then the runs, then the delete set, all LEB128
  varints with UTF-8 text; exact layout in the header comment of
  `CrdtDocNative.cpp`. Runs and ranges are sorted and merged canonically, so
  `encodeState()` depends only on what the document contains, not on how a
  replica happened to fragment its runs.
- **Untrusted input.** Every count and length is checked against the bytes
  left before anything is allocated, every integer overflow is rejected, and
  an update that does not decode changes nothing. An edit that depends on text
  not seen yet waits in a queue capped at 32 M units-plus-run-costs (a
  megabyte-class note is about 1 M); an update that would exceed the cap, or
  is that large itself, is refused whole (`ok` false). The queue wakes edits
  through an index, so a hostile chain of dependencies costs O(n log n), not
  O(n^2) (`AHostileDependencyChainIsLinear`).
- **Position lookup.** A linked list of runs with a cursor at the last edit:
  typing on in one place is O(1) (the run just grows), a jump is O(runs).
- **Interleaving.** Concurrent forward typing at one spot never interleaves
  (each character's origin is the one before it). Concurrent repeated insertion
  at the *same* offset (typing backwards) may interleave; Yrs has the same
  profile. Fugue would fix that; it is not promised or tested.

Known limits, each marked `ponytail:` in the code:

- No garbage collection: tombstones and the run arena are never freed, so a
  long editing history only grows.
- A remote update walks every run (O(runs)) to report what changed; fine for
  notes, an index would be needed for a document of millions of runs.
- Every `diffSince` carries the whole delete set, so a diff is never smaller
  than the history of deletions.
- Text is held as UTF-16 (2 bytes a unit), twice Yrs's UTF-8.
- Remote runs are not re-merged in memory, only when encoded (so after a
  reload); a long remote typing session leaves one small run per update.

### Why a small crate instead of yrs' own C bindings (crate `yffi`)

yffi ships no header and cannot be used as a dependency (it is built only as
a static/dynamic library), and its text API passes NUL-terminated strings and
unwraps: one U+0000 inside a *remote* update would abort the process. `yrs-ffi`
is about 300 lines of Rust over yrs: UTF-8 with explicit lengths, UTF-16
offsets (`OffsetKind::Utf16`), and a `catch_unwind` around every entry point,
so a hostile or corrupt update is an error code. It also refuses an update,
or a peer's state vector, whose claimed entry count the bytes could not hold
(yrs would reserve and touch gigabytes before noticing: five bytes cost about
30 ms each). **Bumping yrs means re-checking that guard** (`plausible()` in
`lib.rs`, noted in `Cargo.toml`): it is the only thing between a size claim in
a decoder and the allocator.

Failure semantics. yrs commits a transaction when it is dropped, and an update
that decodes can still fail to integrate halfway. After such an error (or a
panic) `applyUpdate` reports `ok` false *and* returns the edits for the part
that did land, so the editor stays in step with `text()`; the replica may then
miss edits its peers have, and the caller should resync from
`encodeState()` / `diffSince()`. `localEdit` likewise returns whatever update a
half-done edit produced. Native keeps its stricter meaning (`ok` false changes
nothing), which the same wording allows.

Embeds. A Yjs client can put an embed or a nested type into the text; it takes
one offset in yrs. `text()` spells each as U+FFFC, as the edit stream does, so
the text and every offset reported for it count the same units.

Known limits: editing offsets must not split a surrogate pair, and a lone
surrogate is stored as U+FFFD (one unit); typing at the end of a 1 MB note costs
about 1.3 ms (yrs walks the block holding the position), 0.13 ms at 100 KB.
### A yrs quirk: a pending update that is never retried

Cause found in yrs 0.28.0, not in `yrs-ffi`. When an update arrives before the
edits it depends on, yrs keeps it pending together with a "missing" clock per
client, and retries it on a later update only if the missing clock is below
the store's clock for that client (`TransactionMut::apply_update`, step 3).
But a block that arrives ahead of a gap is integrated at once with a skip
placeholder before it, which moves the store's clock (`get_clock`, the end of
the last block) past the gap, while the missing clock is recorded from that
same `get_clock` (`BlockPicker::switch`, `update.rs`). Concretely: client C
types `x`, then `y` after it, then `w` at the start of the text. A replica
that receives `w`, `y`, `x` integrates `w` (it needs nothing from C), parks
`y` (it needs `x`, inside the gap) with missing clock 3, and when `x` arrives
the clock is 3 and `3 < 3` is false: `y` stays pending, so the text lacks it
and the state vector for C stops at 1 instead of 3. The next update from C
beyond the gap, or a `diffSince` exchange with any peer that has `y`, releases
it. Native does not have this problem.

So a Yrs replica that has received everything can still lag until the peers
exchange state vectors; the sharing layer should do that on connect and
periodically (it needs to for offline peers anyway). `AnEditWaitingBehindALaterOneIsReleased`
is the minimal case; `ThreeReplicasConvergeUnderRandomDelivery` and
`AReloadedReplicaKeepsMergingLikeTheOthers` do the exchange for Yrs only. A
yrs bump that fixes this lets those workarounds go (the minimal test then
passes without the exchange). Not patched here: the fix belongs upstream.

## Building without cargo

Nothing to do: with no cargo on the path, or `-DNOTEAPPC_WITH_SHARE_YRS=OFF`,
`libs/ext/share` still builds with the Native backend alone,
`CrdtDoc::available(Yrs)` is false, and the Yrs rows of the test suite skip.
Android builds are Native only.

## What is fetched

Only by cargo, only the first time the Yrs backend is built (or when cargo's
cache lacks them), and pinned by the committed `libs/ext/share/yrs-ffi/Cargo.lock`
(the build uses `--locked`):

| crate | version | licence |
|---|---|---|
| yrs | 0.28.0 (exact, released 2026-09-17) | MIT |
| 44 more crates it needs | as locked | MIT / Apache-2.0 and similar |

45 crates, 3.5 MB of source in total. The static library is about 13 MB and
adds about 0.8 MB to a stripped test executable (measured against the same executable linked to a stub).

## The link layer (libs/ext/link)

Identity and the peer-to-peer `Node` (libdatachannel data channels, signed
offers, mDNS discovery over avahi) are not written here: they are
Hearth's link layer, vendored. No Qt.

- **Canonical home: Hearth.** The files are listed in Hearth's
  `tools/link-files.txt`: `libs/core/{identity,debuglog}`, `libs/net/{node,tailnet}`,
  their two test files and `proto/link.proto`. They are copied to
  `libs/ext/link/{core,net,proto}` with the include paths unchanged, and
  `libs/ext/link/LINK_SOURCE` records the Hearth commit, its date and the manifest.
  Only `libs/ext/link/CMakeLists.txt` and `LINK_SOURCE` are Notolith's own. A copy,
  not a submodule or a fetch, so both repositories build on their own.
- **Re-vendor:** commit the fix in Hearth first (its tests green), then
  `sh tools/vendor_link.sh <hearth checkout>` and commit
  `chore(link): vendor hearth@<sha>`. The script copies the committed tree at Hearth's
  HEAD, never the working copy, and refuses a Hearth tree with uncommitted changes.
- **The rule: fix in Hearth first, never patch `libs/ext/link/{core,net,proto}`.**
  `sh tools/vendor_link.sh --check <hearth checkout>` diffs the copy (and its file
  set) against the commit in `LINK_SOURCE` and exits 1 on any drift; ctest
  `link_vendor_check` runs it against `NOTEAPPC_HEARTH_DIR` (default
  `~/Documents/AI/projects/hearth`) and skips when there is no checkout there.
- **What Notolith will pass** (wired by the engine lane): `Node::Config`
  `signatureDomain = "notolith"` and `serviceType = "_notolith._tcp"`, so a Notolith
  node and a Hearth node never see or accept each other, and its own debug log via
  `setDebugLogPath()` (left alone, it logs into Hearth's directory).
- **Build:** `NOTEAPPC_WITH_SHARE` (on by default) builds `noteAppC_link`. It needs
  libdatachannel, avahi-client, protobuf, nlohmann_json and libsodium; when any is
  missing the option is silently off and the rest of the app builds as before.
- **Tests:** ctest `link` runs Hearth's `Node.*` and `Tailnet.*` suites unchanged:
  loopback only, the mDNS cases skip without avahi-daemon, `XDG_STATE_HOME` points
  into the build directory. Hearth has seen `Node.DebugLogNamesPeersByPrefixAndLeaksNoMessages`
  hang once in shutdown (23 minutes in `futex_wait`); `link` has a 10 minute ctest
  TIMEOUT so a hang fails instead of blocking.

## The engine (libs/ext/shareengine)

`ShareEngine` is everything about sharing except an editor and a window: it
keeps this device's keys, invites and admits members, and moves CRDT updates
between devices. Built when the CRDT, the link and sync are (`noteAppC_shareengine`),
so sharing needs libsodium and Qt6Keychain, which sync needs for the same two jobs:
sealing, and keeping a key. Nothing is built or read until `start()`: no keyring
access, no socket, no file.

- **Keys.** One keyring entry (service `noteAppC-share`, `SecretStore`, shared with
  sync) holds an Ed25519 key (what the link authenticates, what signs frames and
  member lists) and an X25519 key (what a share key is sealed to). A malformed
  entry is replaced; the pair is proved once on load.
- **Wire** (`share.proto`, every message at most 1 MiB, the link's own cap, checked
  before parsing). `Join` (invite proof, box key, name), `Welcome` (share key sealed
  to the joiner's box key; the member list and the note state sealed under the share
  key), `Rekey` (new key and/or member list), `Refused`, and `Frame`: a `Payload`
  (an update, a state vector, or a diff - the answer to a state vector) sealed under the share key, bound to (share, epoch,
  sender, seq) and signed by the sender. `Frame`, `Welcome` and `Rekey` travel on
  the link's ordered `sync` channel, the rest on `ctl`. A relay would see ids and
  sizes only.
- **Invite** `notolith-share:` + base64url JSON (share id, owner key, 32-byte token,
  `host:port` list, expiry, CRDT backend). The joiner proves the token with a keyed
  hash over its own link key; the owner approves each device by its safety number.
  A new `invite()` voids the previous code; a restart voids open ones. Hosts must be
  loopback, LAN or tailnet addresses.
- **Members** are device keys. The owner signs the list (`MemberList`); every
  member verifies it with the owner's key. `revoke` removes a member, mints a new
  share key, bumps the epoch and sends each remaining member the key sealed to it:
  the revoked device is ignored from then on and keeps what it already has.
- **Sync.** At every connection each side sends its state vector and answers with
  what the other lacks (so offline edits on both sides merge); a received change is
  passed on to the other connected members, once (a second arrival changes nothing),
  so members need not be connected to each other. `seq` counts from 1 per connection.
  A state vector also goes to every connected member every 30 s and 3 s after the last
  change came in (`Config::resyncMs`, `settleMs`): that repairs a lost frame and releases
  an update the yrs backend holds pending behind an edit that has arrived since. The
  answer is a `Diff` payload, which starts no exchange of its own, so the rounds end.
- **Threads.** The engine and every `CrdtDoc` live on the UI thread; link callbacks
  copy their arguments and queue onto it. A keystroke costs one `localEdit`, one AEAD
  seal and one signature per connected member.
- **Persistence.** `<GenericDataLocation>/noteAppC/share/<16 hex of sha256 of the vault
  path>/<share id>/`: `share.json` (key, epoch, signed member list, path, replica id, and on a
  joiner the invite's owner addresses),
  `state.bin` (the CRDT state), `base.md` (the text the file and the CRDT last agreed
  on); files 0600, written on a 2 s debounce, by `persist()` and on destruction. The
  key sits beside a plaintext note: this protects the wire, not the disk.
- **Limits.** A note whose state exceeds 1 MiB cannot be shared yet (refused). The
  engine passes the title through with control and bidi characters removed; making a
  file name of it (`Shared/<title>.md`) is the window's job.

What the engine refuses (each case is a test in `tests/test_share.cpp`):

| from | check | on failure |
|---|---|---|
| any peer | size <= 1 MiB, then parse | dropped, counted |
| `Frame` | known share, sender is a member and the link peer, same epoch, `seq` beyond the last, signature, AEAD, payload | dropped, counted (a wrong epoch reads "waiting for the owner") |
| `Join` | I own the share, an open invite, proof equal (constant time), not expired, same backend, box key size | a wrong proof is dropped silently (no oracle); expired or other backend gets `Refused` |
| `Welcome` | from the owner the invite named, key opens, box opens bound to me, list verifies with that owner and names me, state loads | dropped, `failed()` |
| `Rekey` | from the owner, list verifies, epoch not older, key opens for the same epoch | dropped |
| invite code | prefix, version, sizes, not expired, a backend this build has, private hosts | `join()` false with the reason |
| strangers | never sent anything, frames from non-members dropped; the link itself is identity-authenticated, so ignoring them (not closing) is complete | counted |

Tests: `ctest -R '^share$'` - loopback only, mDNS off, port 0, an in-memory secret
store, temporary store directories.

## In the window (libs/ui/ShareSession, MainWindow)

Settings, Core plugins, "Share a note" (`share`, **off by default**). With it off
nothing exists: no engine, no keys read, no socket, no mDNS, and the `share:*`
commands are disabled. With it on (and a vault open, after the first paint)
the window owns a `ShareEngine` that is built but **not running**; it starts
(keyring, socket) when a note is shared or joined, or at once when the store
already holds shares of this vault.

- **Commands:** `share:share-note` (saves the note, makes the share and opens
  the share dialog with the invitation code; on a note shared already it just
  opens the dialog), `share:who` (the dialog), `share:join` (the join dialog),
  `share:stop` (forgets the share on this device). Under them are
  `MainWindow::shareNote()`, `joinShare()` and `stopSharingNote()`. An
  encrypted (`.mdlock`) note, a note that is not valid UTF-8 and anything but
  a note of the open vault cannot be shared. A device asking in is approved or
  refused with a box naming it and its safety number
  (`MainWindow::setShareApprovalForTests` replaces the box in tests).
- **Share dialog** (`libs/ui/ShareDialog.cpp`, window-modal, follows the
  engine while open): the owner sees the invitation code with Copy and New
  invite (a new code voids the old one; an old code is never shown again).
  Everyone sees this device's fingerprint and each member's name,
  fingerprint, safety number (the same on both devices: read it to each
  other) and status (here, away, this device, owner). The owner can remove a
  device (asked first; it keeps what it has, the key rotates). Stop sharing
  here forgets the share on this device.
- **Join dialog:** the code is parsed as it is typed; what is wrong with it
  (damaged, expired, another CRDT engine, a public address) shows under the
  field and Join stays off until it parses. A refusal from the engine shows
  the same way and the dialog stays open.
- **Status:** the status bar shows "Shared - N here" (connected members
  besides this device) while the active note is shared, with refused
  messages in its tooltip; a shared note's tab carries a `⇄` after its name.
- **The joiner's copy** is written to `Shared/<title>.md` (`Shared/<title> (2).md`
  and so on while that exists) and opened. The title is the owner's string, so
  the file name is made by `ShareSession::fileNameFor`: no `/` or `\`, no
  control characters, no `..`, no leading dots, 100 characters at most (never
  half a surrogate pair), the first 8 hex digits of the share id when nothing
  is left; the write goes through `Vault::writeNoteNoFollow`, so a symlinked
  `Shared/` cannot lead out of the vault.
- **One `ShareSession` per share** the engine knows, alive while the window is,
  whether or not a tab shows the note (closing the tab does not end it). It
  follows the editors of every tab on the note (`MainWindow::syncShareViews`,
  run after `openNote`, a tab closing, a rename and the like):
  - an editor's `edited()` becomes `CrdtDoc::localEdit` plus
    `ShareEngine::broadcast`. The other tabs of the same note get the edit
    from the window's existing fan-out, not from the session. A whole-text
    change (undo, redo, select-all and type) is sent as the difference to the
    document, not as a replacement, so what others typed meanwhile survives
    your undo. An edit that would split a surrogate pair (a one-unit Delete on
    an emoji) is widened to the pair for the document, and the editors then
    follow the document's text, so the two never differ;
  - `ShareEngine::remoteEdits` is made in every editor of the note with
    `applyExternalEdit`, which emits no `edited()`, so nothing loops. The
    editor's undo history is carried over the edit (docs/EDITOR.md "Undo across
    edits from elsewhere"), and the window arms its autosave: a collaborator
    who only reads still gets the file written;
  - a save tells the session what the file holds (`noteSaved`: the engine's
    `persist`, which is also the base for the next fold-in). Any text the
    editors were given without `edited()` (a restore over the note, a reload)
    is reconciled to the document as one local edit (`adoptText`).
- **The file is a writer too.** `ShareSession::foldInDisk(base, disk)` does a
  three-way merge with one hunk on each side: the file's change (`base` ->
  `disk`) is made in the document, translated past what the document gained
  since `base`, and sent. It is used when the file changes under an open
  note (`reloadChangedNotes`, before the "typing wins" branch) and when the
  first tab opens the note after the application was closed (`attach`, with the
  engine's `base()`); what the file gained while closed, say through another
  device's file sync, folds in, then the editor takes the document's text. A
  change that overlaps the document's own edits since `base` is left out
  ("typing wins", file recovery keeps the other version, as for any note).
- **Restart.** The record, the CRDT state and `base.md` are the engine's
  persistence (above). With the feature on and shares in the store the engine
  starts as the vault opens, and a tab opening a shared note attaches to its
  session. A joiner dials the owner again at the addresses its invite named
  (kept in `share.json`, private ones only), so a share comes back over the
  tailnet or another subnet without mDNS. An owner never dials: its members
  come to it. An owner that moved, or came back on another port, is found by
  mDNS on the LAN; elsewhere the joiner stops sharing that note and joins
  again from a new invite (the addresses are kept from the first one only).
- **Cost.** A keystroke is one `localEdit`, one AEAD seal and one signature per
  connected member, and no copy of the text; the text is fetched only when the
  edit touches half of a surrogate pair. State is written on the engine's 2 s
  debounce and on save, never per keystroke.

Tests: `ctest -R '^share_window$'` - two `MainWindow`s on two temporary vaults,
loopback only, mDNS off, port 0, an in-memory secret store, temporary XDG
directories (`tests/test_share_window.cpp`), every case once on Yrs and once
on Native (`ShareEngine::Config::backend`); `ExternalEditTest.*` and
`HunkTest.*` in `tests/test_editor.cpp`.
