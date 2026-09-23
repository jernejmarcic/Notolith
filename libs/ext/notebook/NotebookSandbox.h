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

/// Wraps `program`+`args` for `tier`. Falls back to SandboxTier::Host (and
/// returns that in `*usedTier`, so a caller can tell the caller-visible
/// trust level actually applied) when Bubblewrap was asked for but `bwrap`
/// is not on PATH - the same silent fallback TexCompiler::compile and
/// PluginHost::sandboxArguments already use, logged by the caller rather
/// than failing the launch outright.
void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier);

} // namespace noteAppC::notebook
