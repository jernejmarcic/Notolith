# Running code blocks

A fenced code block with a language gets a run control (▶) at the right of
its header; its output shows under it and is kept, so reopening the note
shows the last result without running anything. `libs/ext/notebook`, the
"Notebooks" switch in Core plugins.

## How a block runs

With the system's own interpreter or compiler, one short process per run,
no kernel to install or keep running (Jernej, 2026-09-24: "Not jupyter
kernel, should use system itself, or in a docker, most efficient way").

| language (fence) | tool | how |
|---|---|---|
| `python`, `py` | python3 | the block, then its last bare expression printed, as a notebook cell does |
| `haskell`, `hs` | ghci | every top-level expression prints its value; a block with `main` runs with runghc |
| `bash`, `sh`, `shell` | bash | |
| `javascript`, `js` | node | |
| `ruby`, `perl`, `lua` | the same | |
| `c`, `cpp`, `rust` | gcc, g++ -std=c++20, rustc | compiled in a temporary directory, then run |
| `java` | java | single-file source launch |

Blocks share nothing: each run is a fresh process, so a later block cannot
use a variable an earlier one set. A language noteAppC has no recipe for
runs through its Jupyter kernel if one is installed; otherwise the block
says which tool is missing.

A run stops after 30 s, or with "Notebooks: Interrupt the kernel". Output
is capped at 1 MB. Colour codes are stripped. An error is named the way its
traceback names it (`ZeroDivisionError: division by zero`).

## Sandbox

By default a block runs under bubblewrap: `/usr` and `/etc` read-only, a
fresh empty `/tmp`, no network, and nothing of the home directory - only
its own temporary directory, which is its `HOME` too. Built with libseccomp,
it also cannot make namespaces or mounts, ptrace, reach the kernel keyring,
load BPF or modules, use io_uring, or type into the terminal (`TIOCSTI`):
those calls answer "operation not permitted". `#| sandbox: host` in
the block asks to run as you, unsandboxed; it is refused unless allowed in
settings.

Docker (or Podman) would be the next tier, for languages not installed on
the machine; neither is installed on starbase-1, so it is not built.

## Where outputs go

In the XDG cache, keyed by language, source and tier, never in the note.
An encrypted note's blocks do not run at all, since their output would be
kept on disk (docs/ENCRYPTION.md).
