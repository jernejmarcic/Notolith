// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// The tiered sandbox: which of the trust levels a block gets before its
// kernel process is launched. Reuses the exact bubblewrap allowlist shape
// libs/render/TexCompiler.cpp and libs/ext/plugins/PluginHost.cpp already
// use (see the comment on wrapCommand) rather than inventing a second one.

#include <QString>
#include <QStringList>

namespace noteAppC::notebook {

/// There is no tier that runs outside the sandbox: every one is bubblewrap
/// with the syscall filter, and without them nothing runs. The numbers are
/// part of the output cache's key and stay as they were.
enum class SandboxTier {
    Bubblewrap = 1, ///< namespaces + an explicit bind allowlist (the default)
    /// A WebAssembly module under `wasmtime run`, nested inside tier 1's
    /// bubblewrap. Only DirectRunner builds this one: wrapCommand knows
    /// nothing of it and treats it as Bubblewrap, so a kernel asked for in
    /// this tier is confined all the same.
    Wasm = 2,
};

/// What a launch needs bound in, beyond the read-only system directories
/// every tier-1 launch gets: the kernel's own runtime directory (connection
/// file and, since transport is always "ipc" here, its five socket files)
/// always read-write, and anything a runnable file embed needs to read (the
/// note/vault directory) read-only.
struct SandboxRequest {
    QString runtimeDir;          ///< rw; must be set for SandboxTier::Bubblewrap
    QStringList readOnlyBinds;   ///< extra ro binds, e.g. the vault root for `![[x.py]]`
    int processes = 256;         ///< at most this many tasks (threads included) run inside
    /// The cgroup's cap on RAM, swap off (sandbox::limitInCgroup): what the cell, its tmpfs files and /dev/shm
    /// can hold together. The per-process rlimits stay; this is the one that adds up.
    qint64 memoryBytes = 1LL << 30;
};

/// What is actually installed, checked once at startup rather than assumed.
struct SandboxAvailability {
    bool bubblewrap = false;
    /// A container tier (gVisor/Firecracker/a container runtime) is
    /// documented, not built: this build never installs a container runtime,
    /// it only reports whether the host has one. True here would
    /// mean such a tier could actually run, not that this build implements
    /// one.
    bool containerRuntime = false;
    /// The WebAssembly tier's runtime (a `wasmtime` CLI; there is no C API
    /// here), needed for SandboxTier::Wasm and for `wat`/`wasm` blocks.
    bool wasmtime = false;
};
SandboxAvailability detectSandboxAvailability();

/// Wraps `program`+`args` in bubblewrap (every tier, Wasm included), in its
/// own pid, net, ipc, uts, cgroup and user namespaces, with the syscall
/// filter. When `bwrap` is not on PATH, or the filter cannot be built,
/// `*outProgram` comes back empty and the caller refuses the run: a block
/// never runs as the user.
///
/// Unless there is no systemd user manager, the line is `env ... systemd-run --user --scope ... bwrap ...`
/// (sandbox::limitInCgroup): the same process, so `*outProgram` is not always bwrap.
///
/// `*filterFD` is the filter's descriptor, for the caller to keep open in the
/// child (sandbox::prepareChild) and close after start; -1 when `*outProgram`
/// is empty.
void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier, int *filterFD);

/// Why a block of `source` is too big to run (empty when it is not): more than `maxBytes` of UTF-8, the
/// setting `notebooks.maxRunnableBytes`. 0 means no limit.
QString oversizeMessage(const QString &source, qint64 maxBytes);

/// What a vault's note is let to bind into a run: `path` canonical, and only
/// when it is inside `vaultRoot` (itself canonical) and that root is neither
/// `/` nor the home directory. Empty otherwise, and the run goes without it.
QString confinedBind(const QString &path, const QString &vaultRoot);


} // namespace noteAppC::notebook
