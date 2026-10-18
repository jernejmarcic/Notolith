#pragma once

// The bubblewrap command line TeX (libs/render) and the diagram tools
// (libs/editor) run under: the system and one directory, nothing else. What
// it hands bwrap besides the allowlist is the syscall filter (SyscallFilter.h).

#include <QString>
#include <QStringList>

namespace noteAppC::sandbox {

struct Command {
    QString program; ///< bwrap; empty when it is not installed: the caller refuses the run, never runs bare
    QStringList arguments;
    int filterFD = -1; ///< SyscallFilter.h's: keepInChild() it, close() it after QProcess::start
};

/// `program arguments` with /usr, /etc and /var/lib/texmf read-only (the
/// rest of the system is not there: no home, no other drive, no socket),
/// `dir` the only writable place and the working directory, HOME, a private
/// /tmp and /proc and /dev, no network, no inherited environment (PATH and
/// HOME, then `environment` as NAME=value entries). `program` is run by the
/// path given. `readOnlyBinds` are further paths (a tool installed outside
/// /usr) bound read-only at the same place.
Command bubblewrapCommand(const QString &dir, const QString &program, const QStringList &arguments,
                          const QStringList &environment = {}, const QStringList &readOnlyBinds = {});

} // namespace noteAppC::sandbox
