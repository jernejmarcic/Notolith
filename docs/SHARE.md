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
