// SPDX-License-Identifier: GPL-3.0-or-later
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
#include <sys/socket.h>
#ifndef AF_XDP
#define AF_XDP 44
#endif
#endif

namespace noteAppC::sandbox {

#ifdef NOTEAPPC_HAVE_SECCOMP
/// The syscall filter every bwrap sandbox here gets (code blocks, plugins,
/// TeX, diagrams, pandoc), as compiled BPF in a memfd for bwrap's `--seccomp`. The
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
    // By name, so a call this architecture has no number for (the legacy ones on AArch64) is skipped: what
    // cannot be called needs no rule.
    for (const char *name : {
             // Other processes' memory, and the kernel's own.
             "ptrace", "process_vm_readv", "process_vm_writev", "pidfd_getfd", "kcmp", "process_madvise",
             "kexec_load", "kexec_file_load", "init_module", "finit_module", "delete_module", "create_module",
             "get_kernel_syms", "query_module", "reboot", "swapon", "swapoff", "acct", "syslog", "quotactl",
             "quotactl_fd", "_sysctl", "nfsservctl", "uselib", "lookup_dcookie",
             // Raw hardware, the clock and the host's names.
             "iopl", "ioperm", "modify_ldt", "vm86", "vm86old", "pciconfig_read", "pciconfig_write",
             "pciconfig_iobase", "settimeofday", "clock_settime", "clock_adjtime", "adjtimex", "sethostname",
             "setdomainname", "memfd_secret",
             // NUMA placement of pages the sandbox does not own.
             "mbind", "migrate_pages", "move_pages", "set_mempolicy", "set_mempolicy_home_node",
             // Rearranging the namespaces bwrap set up, or making new ones:
             // a fresh user namespace hands back every capability inside it.
             "unshare", "setns", "mount", "umount", "umount2", "pivot_root", "chroot", "open_tree",
             "move_mount", "fsopen", "fsconfig", "fsmount", "fspick", "mount_setattr",
             // The kernel keyring is not namespaced; a file handle names a
             // file outside the mount namespace.
             "keyctl", "add_key", "request_key", "open_by_handle_at",
             // Large kernel surfaces no block needs, and the usual stepping stones of a kernel exploit.
             "bpf", "perf_event_open", "userfaultfd", "io_uring_setup", "io_uring_enter", "io_uring_register",
             "fanotify_init", "fanotify_mark",
         }) {
        const int call = seccomp_syscall_resolve_name(name);
        if (call == __NR_SCMP_ERROR)
            continue;
        ok = ok && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), call, 0) == 0;
    }
    // personality(2) switches the execution domain (READ_IMPLICIT_EXEC, ADDR_NO_RANDOMIZE...), which weakens
    // the layout randomisation a kernel exploit has to guess past. Only the plain Linux domain (0) stays
    // allowed. (libseccomp takes one comparison per argument, so the query value 0xffffffff is denied too.)
    if (const int personality = seccomp_syscall_resolve_name("personality"); personality != __NR_SCMP_ERROR)
        ok = ok && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EPERM), personality, 1, SCMP_A0(SCMP_CMP_NE, 0)) == 0;
    // Socket families a cell has no use for. The network namespace isolates inet and unix, but AF_VSOCK
    // reaches the host's VMs, AF_ALG, AF_KEY, AF_PACKET, AF_RDS, AF_TIPC and AF_XDP are kernel code with a long
    // record of bugs, and AF_BLUETOOTH is the host's radio. Docker and Flatpak deny the same set.
    // EAFNOSUPPORT, "this kernel has no such family": what a program that probes for one expects.
    for (const int family : {AF_VSOCK, AF_ALG, AF_KEY, AF_PACKET, AF_RDS, AF_TIPC, AF_XDP, AF_BLUETOOTH})
        ok = ok && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, static_cast<scmp_datum_t>(family))) == 0;
    // Netlink is the exception that proves a use: getifaddrs(3) asks the kernel for the interface list by
    // NETLINK_ROUTE (protocol 0), and libzmq, which every Jupyter kernel runs on, aborts when that socket cannot
    // be made. So protocol 0 stays (read-only in effect: the capability to change anything is dropped, and the
    // namespace is the cell's own, with a loopback), and every other netlink protocol (netfilter, audit,
    // crypto, generic, ...: kernel subsystems a cell never configures) is refused.
    ok = ok
         && seccomp_rule_add(filter, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 2, SCMP_A0(SCMP_CMP_EQ, AF_NETLINK),
                             SCMP_A2(SCMP_CMP_NE, 0))
                == 0;
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
        qWarning("sandbox: the syscall filter could not be built; nothing is run");
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
    qWarning("sandbox: built without libseccomp; nothing is run");
    return -1;
#endif
}

} // namespace noteAppC::sandbox
