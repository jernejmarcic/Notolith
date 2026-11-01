// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// The bubblewrap command line TeX (libs/render), the diagram tools
// (libs/editor) and pandoc run under: the system and one directory, nothing
// else. What it hands bwrap besides the allowlist is the syscall filter
// (SyscallFilter.h), which is mandatory: no bwrap or no filter, no run.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <sys/resource.h>

namespace noteAppC::sandbox {

struct Command {
    QString program; ///< bwrap; empty when it is not installed or the syscall filter could not be built: the caller refuses the run, never runs bare
    QStringList arguments;
    int filterFD = -1; ///< SyscallFilter.h's: keepInChild() it, close() it after QProcess::start
};

/// bwrap arguments binding the few /etc entries the tools need read-only,
/// plus `extra` (names under /etc), each only when it exists. Never all of
/// /etc: a tool that reads any file it sees (TeX's \input) would ship the
/// machine id, hostname or user list.
QStringList etcBinds(const QStringList &extra = {});

/// `program arguments` with /usr, /var/lib/texmf, /var/cache/fontconfig and etcBinds() read-only (the
/// rest of the system is not there: no home, no other drive, no socket),
/// `dir` the only writable place and the working directory, HOME, a private
/// /tmp and /proc and /dev, no network, no inherited environment (PATH and
/// HOME, then `environment` as NAME=value entries). `program` is run by the
/// path given. `readOnlyBinds` are further paths (a tool installed outside
/// /usr) bound read-only at the same place. At most 256 tasks run inside, and
/// /dev/shm and /tmp are 64 MiB each. `memoryBytes` is the cgroup's cap, see limitInCgroup.
/// `command.program` is then `env` (it ends up running bwrap, in the same process).
Command bubblewrapCommand(const QString &dir, const QString &program, const QStringList &arguments,
                          const QStringList &environment = {}, const QStringList &readOnlyBinds = {},
                          qint64 memoryBytes = 1LL << 30);

/// Runs `program arguments` (a complete bwrap line, filter and all) in a cgroup of its own, by
/// `systemd-run --user --scope` in front of it: at most `memoryBytes` of RAM (swap off) and `tasks` tasks for
/// everything inside. This is the only cap on memory that is not per process: the tmpfs pages a sandbox
/// writes (/dev/shm, a work directory on a tmpfs, /tmp, memfds) are charged to the writer's cgroup, so
/// the cap also stops a cell from filling the machine's RAM through them. A scope execs in place: the process
/// is still the one started, with its descriptors (the syscall filter's) and its pid, for the interrupt walk
/// in KernelProcess. `program` becomes `env`. Without systemd-run or a user manager that answers, nothing
/// changes and the rlimits are all there is (said once, in the log).
void limitInCgroup(QString &program, QStringList &arguments, qint64 memoryBytes, int tasks = 256);

/// Whether limitInCgroup has a user manager to ask (probed once, the first time).
bool cgroupLimitsAvailable();
/// For tests: answers "no user manager" (`false`) or "there is one" without asking. forgetCgroupProbe() asks again.
void overrideCgroupLimits(bool available);
/// Forgets that answer, for a test that changes the environment it was found in.
void forgetCgroupProbe();

/// Puts `prlimit --nproc=N --` in front of `program` in a bwrap argument list
/// (the part after the "--" that ends bwrap's options): the process limit,
/// set inside the new user namespace where only the job's own tasks count.
/// Returns false, and changes nothing, when prlimit is not there.
bool limitProcesses(QStringList &bwrapArguments, int processes);

/// Ceilings set in the child between fork and exec, so they hold for bwrap
/// and everything it starts. 0 leaves a limit alone. (The number of
/// processes is not one of them: RLIMIT_NPROC is counted for the whole user,
/// outside the sandbox too, and bwrap could not even start under a desktop's
/// thousands of tasks; it is set inside, see jobProcesses.) `data` is the memory
/// cap for a runtime that reserves far more address space than it uses (a
/// JVM, V8, GHC) and so cannot live under `addressSpace`.
struct Limits {
    rlim_t cpuSeconds = 0;
    rlim_t addressSpace = 0;
    rlim_t data = 0;
    rlim_t fileSize = 0;
    rlim_t openFiles = 0;
};

/// Sets `limits` and lets the syscall filter's descriptor through the exec:
/// the whole of a child process modifier. Async-signal-safe.
void prepareChild(const Limits &limits, int filterFD);

/// What run() found. `failure` is empty when the program ran to the end,
/// `code` then being its exit code (-1 when a signal ended it).
struct Outcome {
    int code = -1;
    QByteArray out, err;
    QString failure; ///< why it did not run to the end: not started, timed out, too much output
};

/// Runs `command` (bubblewrapCommand's, or any other bwrap line with a
/// filter) to the end with `input` on its standard input, blocking: for a
/// worker thread. The wall-clock limit is `timeoutMs` and the output cap
/// `maxOutput` bytes in all; either one kills the whole job (bwrap's
/// --die-with-parent takes the processes inside with it). The environment
/// bwrap itself gets is PATH alone. `mergeChannels` puts the program's
/// standard error into `out`.
Outcome run(const Command &command, const Limits &limits, const QByteArray &input, int timeoutMs,
            qint64 maxOutput, bool mergeChannels = false);

} // namespace noteAppC::sandbox
