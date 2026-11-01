#pragma once

// The seccomp filter every bubblewrap sandbox in noteAppC hands to bwrap:
// code blocks (libs/ext/notebook), plugins (libs/ext/plugins), TeX and the
// diagram tools (libs/render, libs/editor) and pandoc. What it denies and why
// is on compileSyscallFilter in the .cpp. Built with libseccomp, and
// mandatory: when it cannot be built (no libseccomp in this build, or the
// compile failed) addSyscallFilter says so and every caller refuses to run,
// never running on bubblewrap's namespaces alone.

#include <QStringList>

namespace noteAppC::sandbox {

/// Adds `--seccomp N` to a bwrap command line, among its options (before
/// the "--" that ends them). Returns N: an open, close-on-exec descriptor
/// the caller lets through the exec with keepInChild, from its child
/// process modifier, and closes once QProcess::start has returned. -1, and
/// the arguments untouched, when the filter could not be built: the caller
/// must not start the sandbox then.
int addSyscallFilter(QStringList &bwrapArguments);

/// For a child process modifier, between fork and exec: lets `fd` through
/// the exec. Async-signal-safe; does nothing for -1.
void keepInChild(int fd);

} // namespace noteAppC::sandbox
