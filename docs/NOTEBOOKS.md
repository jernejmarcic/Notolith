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
| `c`, `cpp`, `rust` | gcc, g++ -std=c++20, rustc | compiled in a temporary directory, then run; a block with no `main` is only type-checked (`-fsyntax-only`, `rustc --crate-type=lib --emit=metadata`) and says so |
| `java` | java | single-file source launch; a block with no `main` is only compiled with javac |

By default blocks share nothing: each run is a fresh process, so a later
block cannot use a variable an earlier one set. A language noteAppC has no
recipe for runs through its Jupyter kernel if one is installed; otherwise
the block says which tool is missing.

A run stops after 30 s (or `#| timeout:`; a kernel's clock starts when it
begins the block, not while it starts up), or with "Notebooks: Interrupt the
kernel", or a click on the stop square a running block shows where its run
control was. Output is capped at 1 MB. Colour codes are stripped. An error is
named the way its traceback names it (`ZeroDivisionError: division by zero`).

## Jupyter kernels: variables that live on

"Share one kernel between a note's code blocks" in Settings (off by default,
so nothing starts a kernel unasked), or `#| kernel: on` for one block, runs a
block in the language's Jupyter kernel instead of a fresh process. A note gets
one kernel per language, started by its first block and gone when the window
closes: what one block sets, the next one sees, as in a notebook. Another note
has its own kernel, and so does a block that asked for the host tier, so a
sandboxed block never shares state with an unsandboxed one.

The kernel is a client of the Jupyter messaging protocol (ZeroMQ), so any
kernelspec on the machine is a language; `#| kernel: python3` names one
(`kernel: off` forces the system's own tool even with the setting on). What
is installed here is read from the kernelspec directories, nothing runs to
find out. Python needs `ipykernel`.

A block run in a kernel shows its execution count, `[3]`, beside the run
control (`[*]` while it runs again). Commands (all under "Notebooks:"):

| command | does |
|---|---|
| Run the code block at the cursor | Ctrl+Shift+Return |
| Run every code block in this note | top to bottom |
| Run every code block down to the cursor | "run all above" |
| Run every code block from the cursor down | "run all below": the cursor's block and every one after it |
| Interrupt the kernel | the cursor's language, this note only |
| Restart the kernel | forgets that kernel's variables |
| Restart the kernel and run every code block | |
| Clear the output of every code block | also removes what the sidecar kept |
| Create new notebook | see "Jupyter notebooks" below |

A kernel that dies (the code called `exit()`) tells the running block so, and
the next run starts a new one.

### input()

A block run in a kernel can ask for a line: when the code calls `input()` the
kernel sends `input_request` on its stdin channel and a one-line prompt appears
under the block, with the kernel's own prompt text. Enter sends the line back
as `input_reply`; `getpass` asks the same way and the line is masked. Esc
interrupts the run instead of answering, which is Jupyter's
own way out. The prompt is the editor's, so it scrolls with the block. A block outside a kernel
(the system's own one-shot tool) has no terminal behind it, so `input()` there
still raises `EOFError`.

## Completion and documentation

With the caret in a code block whose note already has a running kernel for
that language (the kernel a run of that block would use: the same note, tier
and `#| kernel:`), **Ctrl+Space** lists what the kernel completes at the caret
and **Shift+Tab** shows the documentation of what is under it (Jupyter's own
keys). Nothing starts a kernel for this: with none running, Ctrl+Space says so
and Shift+Tab does what it does everywhere else (outdent a list item).

The completion list is the theme's popup: Up/Down move, Enter or Tab take the
match (it replaces the token the kernel named), Esc closes, as does typing on
- the list is for the caret it was asked at. A single match is inserted at
once, as in Jupyter. The documentation is a small read-only box under the
caret (above it near the screen's edge) that closes on Esc, a click, another
key or the editor losing focus.

Both are `complete_request` / `inspect_request` on the kernel's shell channel,
sent from the kernel thread and answered on the event loop, so nothing waits
on the kernel; a reply for a caret or text that has moved on is dropped, a newer
question of the same kind cancels the older, and a kernel busy for over 4 s
with a long cell is not waited for. Off with the "Notebooks" core plugin (and
with `NOTEAPPC_WITH_NOTEBOOK` off nothing of this is built into the window);
the editor itself only tests a pointer on each key press. Shift+Tab could only
be this because the editor now keeps Tab and Shift+Tab for itself
(`focusNextPrevChild` is false): before, a window with other widgets in it moved
the focus on and neither key reached the editor's list indent either.

## Options in the fence

`#|` lines at the head of a block, in the language's own comment syntax, so
the note stays clean CommonMark anywhere else:

| option | meaning |
|---|---|
| `eval: false` | never runs; no run control |
| `echo: false` | the listing is hidden in live preview (the caret reveals it) |
| `output: false` | it runs and is kept, but its output is not drawn |
| `include: false` | `echo: false` and `output: false` |
| `cache: false` | nothing is kept: a reopened note shows no output |
| `timeout: 5` | stop after 5 s (a kernel is interrupted) |
| `kernel: on`, `off`, `python3` | see above |
| `sandbox: host`, `sandbox: wasm` | see below |
| `implicit: false` | the note's implicit code is not added to this block |

## Implicit code

A note can carry code that every runnable block of a language gets, so no
block repeats its imports or its `main`. It lives in the note's properties,
one key per language, named as the fence names it (`python` and `py` are two
keys; a note using both sets both):

```yaml
---
implicit-python: |
  import math
  import os
implicit-rust: |
  use std::collections::HashMap;
implicit-rust-main: |
  fn main() {
  {{code}}
  }
---
```

- `implicit-<lang>` goes above the block, on a line of its own.
- `implicit-<lang>-main` is a template: the block goes where `{{code}}` is,
  but only when the block has no entry point of its own (the same test as the
  "no main" check, so a block with its own `main` is not wrapped twice). Put
  `{{code}}` at the start of its line: only the first line of the block is
  indented by what stands before it.
- The text counts as part of the block for the "no main" check: imports and a
  block with no `main` is still only checked, a wrapper makes it a program.
- It applies only when you run a block. A reopened note runs nothing, and
  editing the implicit code marks the output of the blocks it applies to
  "Outdated".
- Line numbers in an error are the block's own; an error inside the
  implicit code says `implicit code, line N`. (A compiler's excerpt of the
  source line still shows the number it counted.)
- With a Jupyter kernel the prelude runs once per kernel, silently (it is
  not an `In [n]`), and again after a restart; if it fails, the error is shown
  on the block's result as `implicit code: ...`. A wrapper is applied to
  every block of a kernel language. Kernel tracebacks are not remapped.
- A block with `#| implicit: false` gets none of it. A block with the
  implicit code shows `+implicit` beside its run control. Copy copies the
  block alone. A runnable `![[file.py]]` embed gets the note's implicit code
  too.
- The properties table shows the value read-only: edit it in the source.

## What a block can show

Output is drawn under the block, in the order the code produced it: text
(stdout; stderr and warnings in orange), an error with its traceback, PNG and
JPEG pictures, SVG, Markdown, LaTeX (through the formula renderer), and HTML
(a pandas table, through the same litehtml that draws HTML blocks; without
it the plain-text form). A carriage return rewrites its line, so a progress
counter shows its last value; `clear_output()` and a redrawn `display(...,
display_id=...)` work. Output over 1 MB of text or 16 MB of pictures is cut.

When a block's code changes after it ran, its earlier output stays, dimmed
and marked "Outdated", until it runs again. That memory is the open editor's:
a reopened note shows only output that matches the code as written.

## Sandbox

By default a block runs under bubblewrap: `/usr` and a few `/etc` entries (the
loader cache, fonts, the timezone, the passwd, group and nsswitch files; not the
machine id or hostname) read-only, a
fresh empty `/tmp`, no network, and nothing of the home directory - only
its own temporary directory, which is its `HOME` too. Built with libseccomp,
it also cannot make namespaces or mounts, ptrace, reach the kernel keyring,
load BPF or modules, use io_uring, or type into the terminal (`TIOCSTI`):
those calls answer "operation not permitted". `#| sandbox: host` in
the block asks to run as you, unsandboxed; it is refused unless allowed in
settings.

A note shared with other devices (docs/SHARE.md) holds code anyone in the
share may have written. Its blocks never leave the sandbox, whatever the
setting says, and each one asks before its first run in this app session,
naming the share's other members. This holds while sharing is switched off
or not running too (the share's record says whose text it is); stopping the
share on this device makes it an ordinary note again. Runs stay manual, as everywhere: opening
a note only reads cached output.

`#| sandbox: wasm` runs the block as a WebAssembly module under `wasmtime`
(a CLI subprocess; install it) and is stricter than the default, so no
setting gates it. The whole `wasmtime` process runs inside the tier-1
bubblewrap and seccomp filter; without bubblewrap the block is refused, as in
tier 1. The module sees only the block's own directory, as `/cell`,
and has no network and 1 GiB of memory. The limits:

- `wat` (and `wasm`) blocks are WebAssembly text and are the module
  themselves; they also run in the default tier, with no `/cell` at all.
- Any other language needs a module standing in for its interpreter: the
  setting `notebooks.wasmModules`, `python=/path/python.wasm;ruby=/path/ruby.wasm`
  (a language's aliases count: `py` finds `python=`). The module is run with
  the block's file as its argument, `/cell/cell.py`; a language with no entry
  is refused, saying so. No interpreter module ships with Notolith.
- A runnable embed's folder (`![[x.py]]`) is not given to the guest: wasmtime
  has no read-only directory grant, so it could write into the vault.
- A wasm block never uses a Jupyter kernel.

Docker (or Podman) would be the next tier, for languages not installed on
the machine; neither is installed on starbase-1, so it is not built.

## Jupyter notebooks (.ipynb)

A `.ipynb` in the vault opens as a note when the notebooks feature is on (it
is refused like any other file that is not a note when off). Markdown cells are the text,
code cells are fenced blocks in the notebook's language (from
`metadata.kernelspec`, else `language_info`, else python), raw cells are
```` ```raw ```` fences, and two markdown cells in a row are told apart by an
`<!-- cell -->` line. The stored outputs (stream, execute_result, display_data,
error: text, PNG, HTML, ANSI stripped from tracebacks) go to the output cache
under the key the block looks itself up with, so they show on the block the
moment it appears. They are decoded and written lazily, when that block first
looks itself up - only blocks that are laid out do - not all of them at every
read: 40 stored plots cost 230 ms of a 358 ms open that way. A run or a clear
of the block before then supersedes them; reading the file again re-seeds, so
at each read the file's outputs win, as before. Running a block uses the same
kernel machinery as any other (a notebook shares one kernel per note whatever
the setting says).

It is editable. Saving merges the text into the notebook on disk by aligning
old and new cells (longest common subsequence): an edited cell keeps its `id`,
metadata and outputs, a new cell is empty with a fresh `id`, and everything
outside `cells`, unknown keys included, is untouched. A text that is the one
the file shows writes back the file's exact bytes. Otherwise it is written as
nbformat writes JSON (one-space indent, sorted keys, no ASCII escaping, final
newline), so a notebook in git changes only where it was edited. nbformat 4 only;
anything else is refused with the reason and never touched.

A run is written back. A cell that finishes running (or is cleared) since the
file was read makes the tab modified, and the next save writes that cell's
`outputs` and `execution_count` from the cache: stream, `execute_result`
(the value of the last expression), `display_data` (PNG or JPEG as base64,
SVG, HTML with its plain-text twin, Markdown) and `error`; the count is the
kernel's own `In [n]`, null for a run without a kernel behind it and for a
cleared cell, whose `outputs` is `[]`. Every other cell keeps the file's own
bytes, because what the editor holds of an output is not all the file said
(colour codes, alternative representations): a notebook that was only opened,
or only had other cells run, changes nowhere else. A run that never reached a
kernel (no kernel for the language, it would not start) writes nothing, and a
cell with `#| cache: false` keeps no output to write. Cleared outputs that were
not there to begin with leave the tab as it was.

Two cells with the same code are two results. The output cache's key has always
been the language, the code and the sandbox tier, so a second copy of a cell
shared the first one's output; each block now also counts how many blocks with
the very same code come before it in the note (0 for the first, which leaves
every existing key as it was) and that count is part of the key, not of what
runs. The window, the reader of the file and the save all count the same way.
The count follows the cell's place: a copy inserted above makes the one below
look itself up under the next number, so its output stays on the cell that is
now first until it is run again.

"Create new notebook" makes the next free `Untitled.ipynb` where New note puts a
note, in nbformat 4.5 with one empty code cell (with an `id`) and the kernelspec
of the installed python kernel, or just `language_info.name = python` when there
is none, and opens it.

A markdown cell's `attachments` are shown: reading the notebook gives the editor
a table of the pictures they hold, and `![x](attachment:name)` draws the one
called `name`, as a block or inline in text. Nothing is written for them, and an
edited cell keeps its attachments. The table is one per notebook, so two cells
that attach different pictures under one name show the first cell's in both, and
an attachment without a file extension in its name is not drawn.

What is still not there: outputs that arrive while a cell runs are shown when it
ends, not as they stream; widgets (`application/vnd.jupyter.widget-view+json`)
show their plain-text twin; nbformat 3 and 2 are refused.

The vault index reads a notebook as the text it opens as (every cell, code in
its fences), while the notebooks feature is on: search finds its words with
offsets into that text, the quick switcher lists it, its tags and `[[links]]`
count, and `[[Analysis.ipynb]]` and its backlinks resolve. It answers to its
file name with the extension only, as any file that is not Markdown does in
Obsidian, so `[[Analysis]]` still means `Analysis.md`. With the feature off it
is a plain file again (a link to it resolves, nothing inside it is searched);
switching the feature, or opening a vault whose cache was written the other way,
rescans only what changed kind. `Indexer::setNotebookReader` is the switch: the
index library knows nothing of nbformat, the window hands it a reader. The index
reads the file's JSON, not its outputs, so a notebook full of PNGs costs one
parse, off the UI thread.

## Where outputs go

In the XDG cache, keyed by language, source and tier, never in the note
(Quarto's `freeze` idea: the `.md` stays clean, and the cache is safe to
delete). Reopening a note shows each block's last output without running
anything. An encrypted note's blocks do not run at all, since their output
would be kept on disk (docs/ENCRYPTION.md).

## Runnable files

`![[script.py]]` shows the file as a listing with the same run control and
output below it, in the file's folder (bound read-only in the sandbox). Known
extensions: py, js, sh, rb, pl, lua, c, cpp, hs, java, rs, go.

## Not built

- Tier 2 and 3 sandboxes (gVisor, Firecracker) and the WASM tier for phones:
  none of runsc, firecracker or a wasm runtime is installed.
- Rich Jupyter signatures: completion and documentation show only what the
  kernel returns as plain text, and the list is not filtered further as you
  type.
- A kernel for Haskell or the other languages the system's own tool covers:
  none installed here, so those stay one process per block.
