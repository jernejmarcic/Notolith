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

By default blocks share nothing: each run is a fresh process, so a later
block cannot use a variable an earlier one set. A language noteAppC has no
recipe for runs through its Jupyter kernel if one is installed; otherwise
the block says which tool is missing.

A run stops after 30 s (or `#| timeout:`), or with "Notebooks: Interrupt the
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
| Interrupt the kernel | the cursor's language, this note only |
| Restart the kernel | forgets that kernel's variables |
| Restart the kernel and run every code block | |
| Clear the output of every code block | also removes what the sidecar kept |

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
| `sandbox: host` | see below |

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

## Jupyter notebooks (.ipynb)

A `.ipynb` in the vault opens as a note when the notebooks feature is on (it
is refused like any other file that is not a note when off). Markdown cells are the text,
code cells are fenced blocks in the notebook's language (from
`metadata.kernelspec`, else `language_info`, else python), raw cells are
```` ```raw ```` fences, and two markdown cells in a row are told apart by an
`<!-- cell -->` line. The stored outputs (stream, execute_result, display_data,
error: text, PNG, HTML, ANSI stripped from tracebacks) are put in the output
cache under the key the block looks itself up with, so they show on the block
the moment the note opens, and running a block uses the same kernel machinery
as any other (a notebook shares one kernel per note whatever the setting says).

It is editable. Saving merges the text into the notebook on disk by aligning
old and new cells (longest common subsequence): an edited cell keeps its `id`,
metadata and outputs, a new cell is empty with a fresh `id`, and everything
outside `cells`, unknown keys included, is untouched. A text that is the one
the file shows writes back the file's exact bytes. Otherwise it is written as
nbformat writes JSON (one-space indent, sorted keys, no ASCII escaping, final
newline), so a notebook in git changes only where it was edited. nbformat 4 only;
anything else is refused with the reason and never touched.

Ceilings: a run's new output is not written back into the file (it lives in the
cache like every block's; the notebook keeps the outputs it came with), because
a block's cache key is its source and two cells with the same code would
collide. `execution_count` is the file's own. Cell attachments are kept but not
shown. The vault index (links, backlinks, search) does not read notebooks: it
lists only Markdown files.

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
- Writing a run's output back into an `.ipynb`.
- A kernel for Haskell or the other languages the system's own tool covers:
  none installed here, so those stay one process per block.
