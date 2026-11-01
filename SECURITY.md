# Security

Notolith 0.1.0-beta.1 is a public beta.

## Reporting a vulnerability

Report it privately through GitHub security advisories:
https://github.com/jernejmarcic/Notolith/security/advisories/new

Please do not open a public issue for a vulnerability. If you cannot use
advisories, email jernej.marcic@pm.me. This is a one-person project.

## What the sandbox covers

Code blocks, Jupyter kernels, diagram renderers (Mermaid, PlantUML, Graphviz),
TeX, pandoc and plugins run under bubblewrap with a seccomp filter. Inside:
read-only `/usr` and a few `/etc` entries, an empty temporary `/tmp`, no
network, no view of your home directory or the vault, CPU, memory, file and
time limits. If `bwrap` or libseccomp is missing, they refuse to run; there is
no unsandboxed fallback. A vault is untrusted until you trust it, and nothing
runs without a click. Details: `docs/NOTEBOOKS.md#sandbox`.

## What it does not cover

- It is a Linux namespace and syscall sandbox, not a virtual machine. A kernel
  bug that escapes namespaces or the filter escapes it.
- A trusted vault, or a block you choose to run, can still burn CPU and memory
  within the limits, and write inside its own temporary directory.
- Opening a link, a file card or a file with the default app hands the target
  to your desktop, which is outside the sandbox. In an untrusted vault, a file
  card or link inside a note asks before it opens a file; the file tree's
  "Open in default app" is your own request and does not ask. Nothing that is
  a program or a launcher is ever opened, trusted vault or not.
- Tools you point it at (`pandoc`, `latex`, a Jupyter kernel) are as safe as
  their own parsers; the sandbox limits the damage, not the bug.
- Plugins are a preview and the plugin format may change.

## Not in the default build

The default build makes no network connections and has no encryption
(`docs/OFFLINE.md`). Encrypted notes, sync and note sharing are CMake options,
previews, and their cryptography has not been independently audited. Do not
rely on them to protect anything that matters yet.
