# Sharing a note (in progress)

Live co-editing of one note between people, peer to peer. This page covers the
parts that exist: the text CRDT, the link layer and the engine. Wiring it to the
editor and the window comes in a later lane (the plan is in the project TODO).

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

Updates are opaque bytes (the Yjs v1 encoding). Nothing about the engine
behind the interface appears in it.

## Backends

Two are planned behind the same interface and the same test suite
(`tests/test_share_crdt.cpp`, one parameterised suite over `CrdtDoc::Backend`;
a backend the build lacks skips):

- **Yrs** - the Rust [yrs](https://github.com/y-crdt/y-crdt) library, built by
  cargo from `libs/ext/share/yrs-ffi` into a static library and wrapped by
  `CrdtDocYrs.cpp`. Built when cargo is found and `NOTEAPPC_WITH_SHARE_YRS` is
  on (the default); otherwise it is silently left out.
- **Native** - a C++ implementation of the same contract. Not written yet;
  `available(Native)` is false.

### Why a small crate instead of yrs' own C bindings (crate `yffi`)

yffi ships no header and cannot be used as a dependency (it is built only as
a static/dynamic library), and its text API passes NUL-terminated strings and
unwraps: one U+0000 inside a *remote* update would abort the process. `yrs-ffi`
is about 250 lines of Rust over yrs: UTF-8 with explicit lengths, UTF-16
offsets (`OffsetKind::Utf16`), and a `catch_unwind` around every entry point,
so a hostile or corrupt update is an error code. It also refuses an update
whose claimed client count the bytes could not hold (yrs would reserve and
touch gigabytes before noticing).

Known limits: editing offsets must not split a surrogate pair, and a lone
surrogate is stored as U+FFFD (one unit); typing at the end of a 1 MB note costs
about 1.3 ms (yrs walks the block holding the position), 0.13 ms at 100 KB.

## Building without cargo

Nothing to do: with no cargo on the path, or `-DNOTEAPPC_WITH_SHARE_YRS=OFF`,
`libs/ext/share` is not configured, the test is not registered and the rest of
the app builds as before. (Until the Native backend exists, there is no
sharing without cargo.)

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
  (an update or a state vector) sealed under the share key, bound to (share, epoch,
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
- **Threads.** The engine and every `CrdtDoc` live on the UI thread; link callbacks
  copy their arguments and queue onto it. A keystroke costs one `localEdit`, one AEAD
  seal and one signature per connected member.
- **Persistence.** `<GenericDataLocation>/noteAppC/share/<16 hex of sha256 of the vault
  path>/<share id>/`: `share.json` (key, epoch, signed member list, path, replica id),
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
