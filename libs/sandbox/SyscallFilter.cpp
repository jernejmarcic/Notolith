#include "SyscallFilter.h"

#include <QDebug>

#include <fcntl.h>
#include <unistd.h>

#ifdef NOTEAPPC_HAVE_SECCOMP
#include <cerrno>
#include <sched.h>
#include <seccomp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

namespace noteAppC::sandbox {

#ifdef NOTEAPPC_HAVE_SECCOMP
/// The syscall filter every bwrap sandbox here gets (code blocks, plugins,
/// TeX), as compiled BPF in a memfd for bwrap's `--seccomp`. The
/// namespaces already decide what the sandboxed program can see; this
/// takes away the kernel interfaces it has no use for and that are the
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
        qWarning("sandbox: the syscall filter could not be built; bubblewrap alone");
    return fd;
}
#endif


void keepInChild(int fd)
{
    if (fd >= 0)
        fcntl(fd, F_SETFD, 0);
}

int addSyscallFilter(QStringList &bwrapArguments)
{
#ifdef NOTEAPPC_HAVE_SECCOMP
    // Loaded by bwrap last, after its own mounts, just before the exec, so
    // it goes with bwrap's options - before the "--" that ends them.
    const qsizetype end = bwrapArguments.indexOf(QStringLiteral("--"));
    if (end < 0)
        return -1;
    const int fd = compileSyscallFilter();
    if (fd >= 0) {
        bwrapArguments.insert(end, QString::number(fd));
        bwrapArguments.insert(end, QStringLiteral("--seccomp"));
    }
    return fd;
#else
    Q_UNUSED(bwrapArguments);
    return -1;
#endif
}

} // namespace noteAppC::sandbox
