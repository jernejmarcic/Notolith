#include "NotebookSandbox.h"

#include "Bubblewrap.h"
#include "SyscallFilter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace noteAppC::notebook {


SandboxAvailability detectSandboxAvailability()
{
    SandboxAvailability availability;
    availability.bubblewrap = !QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty();
    availability.containerRuntime =
        !QStandardPaths::findExecutable(QStringLiteral("podman")).isEmpty()
        || !QStandardPaths::findExecutable(QStringLiteral("docker")).isEmpty();
    availability.wasmtime = !QStandardPaths::findExecutable(QStringLiteral("wasmtime")).isEmpty();
    return availability;
}

void wrapCommand(SandboxTier tier, const QString &program, const QStringList &args,
                 const SandboxRequest &request, QString *outProgram, QStringList *outArgs,
                 SandboxTier *usedTier, int *filterFD)
{
    *filterFD = -1;
    *usedTier = tier == SandboxTier::Wasm ? SandboxTier::Wasm : SandboxTier::Bubblewrap;
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (bwrap.isEmpty()) {
        // Refused, never quietly run as the user.
        outProgram->clear();
        outArgs->clear();
        return;
    }

    // The same allowlist TexCompiler::compile and PluginHost::sandboxArguments
    // build: /usr and a few /etc entries read-only, /bin, /lib, /lib64 and /sbin as
    // symlinks into /usr (Arch's merged-usr layout, which is what both of
    // those already assume), /proc and /dev handed out fresh, /tmp an empty
    // tmpfs of a fixed size. What is specific to a kernel: transport is always "ipc" (see
    // KernelProcess), so there is no TCP port to reach even on the host -
    // --unshare-net costs the kernel nothing it was using and denies it outright.
    // /etc is the few entries sandbox::etcBinds names, not all of it: a cell
    // is user-run code, but the machine id and hostname still have no
    // business in a note's saved output. Added for a cell: the timezone
    // and the passwd/group/nsswitch trio, without which `whoami`, `ls -l`
    // and getpwuid() in a cell fail on a uid with no name.
    QStringList wrapped = sandbox::etcBinds({QStringLiteral("localtime"), QStringLiteral("passwd"), QStringLiteral("group"),
                                             QStringLiteral("nsswitch.conf")});
    wrapped << QStringList{
        QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--size"), QStringLiteral("67108864"), QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
    };
    for (const QString &path : request.readOnlyBinds)
        wrapped << QStringLiteral("--ro-bind") << path << path;
    // Bound last, and read-write: the connection file and the five ipc
    // socket files the kernel creates in it. Bound after --tmpfs /tmp so it
    // is never buried under that mount even when it happens to sit under
    // /tmp itself - the same ordering note PluginHost::sandboxArguments
    // leaves for its own late binds, and for the same reason.
    wrapped << QStringLiteral("--bind") << request.runtimeDir << request.runtimeDir;

    // Its own pid namespace too: a cell sees its own processes and nothing of the host's (no
    // /proc/<pid>/environ of the app or the session, no /proc/<pid>/root). bwrap runs a small init as pid 1
    // there and the cell below it, so a signal for the cell goes to it by way of /proc (see
    // KernelProcess::interrupt); killing bwrap takes the namespace with it (--die-with-parent).
    // The user namespace is required, not best effort, and nothing inside can make another.
    wrapped << QStringLiteral("--unshare-user") << QStringLiteral("--disable-userns") << QStringLiteral("--unshare-ipc")
            << QStringLiteral("--unshare-uts") << QStringLiteral("--unshare-cgroup") << QStringLiteral("--unshare-pid")
            << QStringLiteral("--unshare-net") << QStringLiteral("--cap-drop") << QStringLiteral("ALL")
            << QStringLiteral("--clearenv") << QStringLiteral("--setenv") << QStringLiteral("PATH")
            << QStringLiteral("/usr/bin:/bin")
            // No real HOME is bound; a kernel that insists on one (IPython's
            // profile directory, matplotlib's config cache) gets the runtime
            // directory instead, already read-write and private to this run.
            << QStringLiteral("--setenv") << QStringLiteral("HOME") << request.runtimeDir
            << QStringLiteral("--die-with-parent") << QStringLiteral("--new-session")
            << QStringLiteral("--chdir") << request.runtimeDir;
    wrapped << QStringLiteral("--") << program;
    wrapped += args;
    // No filter or no process limit, no run.
    if (!sandbox::limitProcesses(wrapped, request.processes) || (*filterFD = sandbox::addSyscallFilter(wrapped)) < 0) {
        outProgram->clear();
        outArgs->clear();
        return;
    }
    *outProgram = bwrap;
    *outArgs = wrapped;
}

QString oversizeMessage(const QString &source, qint64 maxBytes)
{
    const qint64 bytes = source.toUtf8().size();
    if (maxBytes <= 0 || bytes <= maxBytes)
        return {};
    return QCoreApplication::translate("NotebookSandbox",
                                       "Not run: this block is %1 KiB, over the %2 KiB limit (Settings: notebooks.maxRunnableBytes, 0 lifts it)")
        .arg((bytes + 1023) / 1024)
        .arg((maxBytes + 1023) / 1024);
}

QString confinedBind(const QString &path, const QString &vaultRoot)
{
    const QString real = QFileInfo(path).canonicalFilePath();
    const QString root = QFileInfo(vaultRoot).canonicalFilePath();
    if (real.isEmpty() || root.isEmpty() || root == QLatin1String("/") || root == QDir::home().canonicalPath())
        return {};
    return real == root || real.startsWith(root + QLatin1Char('/')) ? real : QString();
}

} // namespace noteAppC::notebook
