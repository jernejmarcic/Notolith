# Sharing a note (in progress)

Live co-editing of one note between people, peer to peer. This page covers the
part that exists: the text CRDT. The link layer, the engine and the UI come in
later lanes (the plan is in the project TODO).

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
is about 250 lines of Rust over yrs: UTF-8 with explicit lengths, UTF-16
offsets (`OffsetKind::Utf16`), and a `catch_unwind` around every entry point,
so a hostile or corrupt update is an error code. It also refuses an update
whose claimed client count the bytes could not hold (yrs would reserve and
touch gigabytes before noticing).

Known limits: editing offsets must not split a surrogate pair, and a lone
surrogate is stored as U+FFFD (one unit); typing at the end of a 1 MB note costs
about 1.3 ms (yrs walks the block holding the position), 0.13 ms at 100 KB.
Also observed: after updates delivered in a shuffled order, yrs can still hold
one pending although the edit it waited for has arrived; exchanging state
vectors (`diffSince`) releases it (`ThreeReplicasConvergeUnderRandomDelivery`
does that for Yrs only, comment there).

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
