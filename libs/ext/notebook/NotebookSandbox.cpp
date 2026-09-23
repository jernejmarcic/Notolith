#include "NotebookSandbox.h"

#include <QStandardPaths>

namespace noteAppC::notebook {

SandboxAvailability detectSandboxAvailability()
{
    SandboxAvailability availability;
    availability.bubblewrap = !QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty();
    availability.containerRuntime =
        !QStandardPaths::findExecutable(QStringLiteral("podman")).isEmpty()
        || !QStandardPaths::findExecutable(QStringLiteral("docker")).isEmpty();
    return availability;
}

void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier)
{
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (tier != SandboxTier::Bubblewrap || bwrap.isEmpty()) {
        *outProgram = program;
        *outArgs = args;
        *usedTier = SandboxTier::Host;
        return;
    }

    // The same allowlist TexCompiler::compile and PluginHost::sandboxArguments
    // build: /usr and /etc read-only, /bin, /lib, /lib64 and /sbin as
    // symlinks into /usr (Arch's merged-usr layout, which is what both of
    // those already assume), /proc and /dev handed out fresh, /tmp an empty
    // tmpfs. What is specific to a kernel: transport is always "ipc" (see
    // KernelProcess), so there is no TCP port to reach even on the host -
    // --unshare-net costs the kernel nothing it was using and denies it
    // outright, which TexCompiler cannot do (a TeX engine may need to reach
    // a proxy) and PluginHost only does when a plugin was not granted the
    // network permission.
    QStringList wrapped = {
        QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
        QStringLiteral("--ro-bind"), QStringLiteral("/etc"), QStringLiteral("/etc"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
    };
    for (const QString &path : request.readOnlyBinds)
        wrapped << QStringLiteral("--ro-bind") << path << path;
    // Bound last, and read-write: the connection file and the five ipc
    // socket files the kernel creates in it. Bound after --tmpfs /tmp so it
    // is never buried under that mount even when it happens to sit under
    // /tmp itself - the same ordering note PluginHost::sandboxArguments
    // leaves for its own late binds, and for the same reason.
    wrapped << QStringLiteral("--bind") << request.runtimeDir << request.runtimeDir;

    wrapped << QStringLiteral("--unshare-user-try") << QStringLiteral("--unshare-ipc")
           << QStringLiteral("--unshare-pid") << QStringLiteral("--unshare-uts")
           << QStringLiteral("--unshare-cgroup-try") << QStringLiteral("--unshare-net")
           << QStringLiteral("--clearenv") << QStringLiteral("--setenv") << QStringLiteral("PATH")
           << QStringLiteral("/usr/bin:/bin")
           // No real HOME is bound; a kernel that insists on one (IPython's
           // profile directory, matplotlib's config cache) gets the runtime
           // directory instead, already read-write and private to this run.
           << QStringLiteral("--setenv") << QStringLiteral("HOME") << request.runtimeDir
           << QStringLiteral("--die-with-parent") << QStringLiteral("--new-session")
           << QStringLiteral("--chdir") << request.runtimeDir << QStringLiteral("--") << program;
    wrapped += args;

    *outProgram = bwrap;
    *outArgs = wrapped;
    *usedTier = SandboxTier::Bubblewrap;
}

} // namespace noteAppC::notebook
