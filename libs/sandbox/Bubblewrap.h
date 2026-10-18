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

/// bwrap arguments binding the few /etc entries the tools need read-only,
/// plus `extra` (names under /etc), each only when it exists. Never all of
/// /etc: a tool that reads any file it sees (TeX's \input) would ship the
/// machine id, hostname or user list.
QStringList etcBinds(const QStringList &extra = {});

/// `program arguments` with /usr, /var/lib/texmf and etcBinds() read-only (the
/// rest of the system is not there: no home, no other drive, no socket),
/// `dir` the only writable place and the working directory, HOME, a private
/// /tmp and /proc and /dev, no network, no inherited environment (PATH and
/// HOME, then `environment` as NAME=value entries). `program` is run by the
/// path given. `readOnlyBinds` are further paths (a tool installed outside
/// /usr) bound read-only at the same place.
Command bubblewrapCommand(const QString &dir, const QString &program, const QStringList &arguments,
                          const QStringList &environment = {}, const QStringList &readOnlyBinds = {});

} // namespace noteAppC::sandbox
