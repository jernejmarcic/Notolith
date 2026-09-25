#include "NotebookSandbox.h"

#include <QDebug>
#include <QStandardPaths>

#include <fcntl.h>
#include <unistd.h>

#ifdef NOTEAPPC_HAVE_SECCOMP
#include <cerrno>
#include <sched.h>
#include <seccomp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

namespace noteAppC::notebook {

#ifdef NOTEAPPC_HAVE_SECCOMP
/// Tier 1's syscall filter, as compiled BPF in a memfd for bwrap's
/// `--seccomp`. The namespaces already decide what a block can see; this
/// takes away the kernel interfaces a block has no use for and that are the
/// usual way out of a namespace sandbox - the same idea as Flatpak's and
/// Docker's default profiles, and like theirs a denylist answering EPERM
/// (so a program that probes for a feature gets a clean "no", not a kill).
/// libseccomp's default for a foreign architecture is to kill, so a 32-bit
/// syscall table cannot be used to step around the list.
///
/// Landlock is left out on purpose: the mount namespace already limits the
/// filesystem to the bind allowlist, and the network namespace has no
/// network, which is everything Landlock would restrict here.
static int compileSyscallFilter()
{
    scmp_filter_ctx filter = seccomp_init(SCMP_ACT_ALLOW);
    if (!filter)
        return -1;
    bool ok = true;
    for (const int call : {
             // Other processes' memory, and the kernel's own.
             SCMP_SYS(ptrace), SCMP_SYS(process_vm_readv), SCMP_SYS(process_vm_writev),
             SCMP_SYS(kexec_load), SCMP_SYS(kexec_file_load), SCMP_SYS(init_module),
             SCMP_SYS(finit_module), SCMP_SYS(delete_module), SCMP_SYS(reboot),
             SCMP_SYS(swapon), SCMP_SYS(swapoff), SCMP_SYS(acct), SCMP_SYS(syslog),
             SCMP_SYS(quotactl),
             // Rearranging the namespaces bwrap set up, or making new ones:
             // a fresh user namespace hands back every capability inside it.
             SCMP_SYS(unshare), SCMP_SYS(setns), SCMP_SYS(mount), SCMP_SYS(umount2),
             SCMP_SYS(pivot_root), SCMP_SYS(chroot), SCMP_SYS(open_tree), SCMP_SYS(move_mount),
             SCMP_SYS(fsopen), SCMP_SYS(fsconfig), SCMP_SYS(fsmount), SCMP_SYS(fspick),
             // The kernel keyring is not namespaced; a file handle names a
             // file outside the mount namespace.
             SCMP_SYS(keyctl), SCMP_SYS(add_key), SCMP_SYS(request_key),
             SCMP_SYS(open_by_handle_at),
             // Large kernel surfaces no block needs.
             SCMP_SYS(bpf), SCMP_SYS(perf_event_open), SCMP_SYS(userfaultfd),
             SCMP_SYS(io_uring_setup), SCMP_SYS(io_uring_enter), SCMP_SYS(io_uring_register),
         })
        ok = ok && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), call, 0) == 0;
    // clone(2) with CLONE_NEWUSER is unshare by another name. clone3 passes
    // its flags in a struct seccomp cannot read, so it answers ENOSYS and
    // glibc falls back to clone, which is checked. The flags are clone's
    // first argument on x86-64 and AArch64, the two targets built.
    ok = ok
         && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), 1,
                             SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER))
                == 0
         && seccomp_rule_add(filter, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0) == 0
         // Typing into the terminal noteAppC was started from; --new-session
         // already detaches it, this is the belt to those braces.
         && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ioctl), 1,
                             SCMP_A1(SCMP_CMP_MASKED_EQ, 0xFFFFFFFFu, TIOCSTI))
                == 0
         && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ioctl), 1,
                             SCMP_A1(SCMP_CMP_MASKED_EQ, 0xFFFFFFFFu, TIOCLINUX))
                == 0;

    int fd = ok ? memfd_create("noteAppC-seccomp", MFD_CLOEXEC) : -1;
    if (fd >= 0 && (seccomp_export_bpf(filter, fd) != 0 || lseek(fd, 0, SEEK_SET) != 0)) {
        close(fd);
        fd = -1;
    }
    seccomp_release(filter);
    if (fd < 0)
        qWarning("notebook: the tier 1 syscall filter could not be built; bubblewrap alone");
    return fd;
}
#endif

void keepInChild(int fd)
{
    if (fd >= 0)
        fcntl(fd, F_SETFD, 0);
}

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
                 SandboxTier *usedTier, int *filterFD)
{
    if (filterFD)
        *filterFD = -1;
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
#ifdef NOTEAPPC_HAVE_SECCOMP
    // Loaded by bwrap last, after its own mounts, just before the exec.
    if (filterFD && (*filterFD = compileSyscallFilter()) >= 0)
        wrapped << QStringLiteral("--seccomp") << QString::number(*filterFD);
#endif
    wrapped << QStringLiteral("--") << program;
    wrapped += args;

    *outProgram = bwrap;
    *outArgs = wrapped;
    *usedTier = SandboxTier::Bubblewrap;
}

} // namespace noteAppC::notebook
