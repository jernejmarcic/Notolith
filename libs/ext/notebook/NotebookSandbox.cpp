#include "NotebookSandbox.h"

#include "Bubblewrap.h"
#include "SyscallFilter.h"

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
    if (filterFD)
        *filterFD = -1;
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (tier == SandboxTier::Host) {
        *outProgram = program;
        *outArgs = args;
        *usedTier = SandboxTier::Host;
        return;
    }
    if (bwrap.isEmpty()) {
        // Refused, never quietly run as the user: a block that did not ask
        // for tier 0 does not get it.
        outProgram->clear();
        outArgs->clear();
        *usedTier = SandboxTier::Bubblewrap;
        return;
    }

    // The same allowlist TexCompiler::compile and PluginHost::sandboxArguments
    // build: /usr and a few /etc entries read-only, /bin, /lib, /lib64 and /sbin as
    // symlinks into /usr (Arch's merged-usr layout, which is what both of
    // those already assume), /proc and /dev handed out fresh, /tmp an empty
    // tmpfs. What is specific to a kernel: transport is always "ipc" (see
    // KernelProcess), so there is no TCP port to reach even on the host -
    // --unshare-net costs the kernel nothing it was using and denies it
    // outright, which TexCompiler cannot do (a TeX engine may need to reach
    // a proxy) and PluginHost only does when a plugin was not granted the
    // network permission.
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

    // No --unshare-pid, unlike PluginHost's sandbox: bwrap(1) says that with
    // a pid namespace it "runs a minimal pid 1 process in the sandbox...
    // detects when the initial application process (pid 2) dies and reports
    // its exit status back to the original spawner" - reporting an exit
    // status is all it documents doing; it does not forward signals sent to
    // the outer process into the namespace. KernelProcess::interrupt() and
    // stop() both rely on the pid QProcess sees being the kernel itself, not
    // an unrelated reaper one level removed - without --unshare-pid, and
    // with no other fork in the way, bwrap execs the kernel in its own
    // place, so that pid *is* the kernel. The cost is real but small: a
    // sandboxed kernel can see (not touch - the other unshares still stand)
    // the host's process list.
    wrapped << QStringLiteral("--unshare-user-try") << QStringLiteral("--unshare-ipc")
           << QStringLiteral("--unshare-uts")
           << QStringLiteral("--unshare-cgroup-try") << QStringLiteral("--unshare-net")
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
    if (filterFD)
        *filterFD = sandbox::addSyscallFilter(wrapped);

    *outProgram = bwrap;
    *outArgs = wrapped;
    *usedTier = SandboxTier::Bubblewrap;
}

} // namespace noteAppC::notebook
