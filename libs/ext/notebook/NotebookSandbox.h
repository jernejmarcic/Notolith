#pragma once

// The tiered sandbox: which of the trust levels a block gets before its
// kernel process is launched. Reuses the exact bubblewrap allowlist shape
// libs/render/TexCompiler.cpp and libs/ext/plugins/PluginHost.cpp already
// use (see the comment on wrapCommand) rather than inventing a second one.

#include <QString>
#include <QStringList>

namespace noteAppC::notebook {

enum class SandboxTier {
    Host = 0,       ///< no sandbox: the kernel runs as the user, full filesystem access
    Bubblewrap = 1, ///< namespaces + an explicit bind allowlist (the desktop default)
    /// A WebAssembly module under `wasmtime run`, nested inside tier 1's
    /// bubblewrap when it is installed. Only DirectRunner builds this one:
    /// wrapCommand knows nothing of it and treats it as Bubblewrap, so a
    /// kernel asked for in this tier is confined, never run as the user.
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

/// Wraps `program`+`args` for `tier`. Only SandboxTier::Host is left alone;
/// every other tier (Wasm included) gets bubblewrap. When Bubblewrap was asked for and
/// `bwrap` is not on PATH, `*outProgram` comes back empty and the caller
/// refuses the run: a block never falls back to running as the user unless
/// it asked for SandboxTier::Host (which Settings has to allow).
///
/// With `filterFD` set, tier 1 also gets the syscall filter
/// (sandbox::addSyscallFilter): `*filterFD` is its descriptor, for the
/// caller to keep open in the child and close after start; -1 when there is
/// no filter (a build without libseccomp, or tier 0).
void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier, int *filterFD = nullptr);


} // namespace noteAppC::notebook
