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

/// What is actually installed, checked once at startup rather than assumed -
/// stage 13's "check what is installed before building on it".
struct SandboxAvailability {
    bool bubblewrap = false;
    /// Tier 2 (gVisor/Firecracker/a container runtime) is documented, not
    /// built: neither podman nor docker is installed on the machine this
    /// lane was built on, and the requirement says leave what is missing for
    /// Jernej rather than install anything. True here would mean a future
    /// tier 2 could actually run, not that this build implements one.
    bool containerRuntime = false;
};
SandboxAvailability detectSandboxAvailability();

/// Wraps `program`+`args` for `tier`. When Bubblewrap was asked for and
/// `bwrap` is not on PATH, `*outProgram` comes back empty and the caller
/// refuses the run: a block never falls back to running as the user unless
/// it asked for SandboxTier::Host (which Settings has to allow).
///
/// With `filterFD` set, tier 1 also gets a syscall filter (see
/// compileSyscallFilter in the .cpp), handed to bwrap as `--seccomp N`:
/// `*filterFD` is then an open, close-on-exec descriptor the caller keeps
/// open in the child (keepInChild, from its child process modifier) and
/// closes once QProcess::start has returned; -1 when there is no filter
/// (a build without libseccomp, or tier 0).
void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier, int *filterFD = nullptr);

/// For a child process modifier, between fork and exec: lets `fd` through
/// the exec. Async-signal-safe; does nothing for -1.
void keepInChild(int fd);

} // namespace noteAppC::notebook
