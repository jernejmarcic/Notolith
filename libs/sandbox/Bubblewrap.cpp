// SPDX-License-Identifier: GPL-3.0-or-later
#include "Bubblewrap.h"

#include "SyscallFilter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <atomic>

#include <unistd.h>

namespace noteAppC::sandbox {

namespace {
std::atomic<int> scopeState{0}; // 0 not asked yet, 1 a user manager takes scopes, 2 none does

/// `env XDG_RUNTIME_DIR=.. DBUS_SESSION_BUS_ADDRESS=.. systemd-run --user --scope -q`, or nothing without
/// systemd-run. bwrap and its callers run with a bare environment, and systemd-run finds the user manager by
/// these two variables, so they go on the command line (bwrap's --clearenv drops them inside).
QStringList scopePrefix()
{
    const QString systemdRun = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
    const QString env = QStandardPaths::findExecutable(QStringLiteral("env"));
    if (systemdRun.isEmpty() || env.isEmpty())
        return {};
    // The runtime directory that has the user manager's bus: $XDG_RUNTIME_DIR, else the standard one.
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString standard = QStringLiteral("/run/user/%1").arg(getuid());
    if (!QFileInfo::exists(runtime + QStringLiteral("/bus")) && QFileInfo::exists(standard + QStringLiteral("/bus")))
        runtime = standard;
    QStringList prefix = {env};
    if (!runtime.isEmpty())
        prefix << QStringLiteral("XDG_RUNTIME_DIR=") + runtime;
    if (const QString bus = qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"); !bus.isEmpty())
        prefix << QStringLiteral("DBUS_SESSION_BUS_ADDRESS=") + bus;
    prefix << systemdRun << QStringLiteral("--user") << QStringLiteral("--scope") << QStringLiteral("-q");
    return prefix;
}

/// Asked once, by starting a scope with a 1 MiB memory cap and reading the cap back from the scope's own cgroup:
/// whether systemd-run is installed, a user manager answers and its memory controller is really there. A scope
/// that merely starts proves nothing (cgroup v1, or a manager without the memory controller, take the property
/// and enforce nothing).
bool scopesAvailable()
{
    int state = scopeState.load();
    if (state != 0)
        return state == 1;
    QStringList probe = scopePrefix();
    bool ok = false;
    if (!probe.isEmpty()) {
        probe << QStringLiteral("-p") << QStringLiteral("MemoryMax=1048576") << QStringLiteral("--")
              << QStringLiteral("/bin/sh") << QStringLiteral("-c")
              << QStringLiteral("cat /sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup | head -n 1)/memory.max");
        QProcess process;
        QProcessEnvironment bare;
        bare.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        process.setProcessEnvironment(bare);
        const QString program = probe.takeFirst();
        process.start(program, probe);
        ok = process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
            && process.readAllStandardOutput().trimmed() == "1048576";
        if (process.state() != QProcess::NotRunning)
            process.kill();
    }
    int expected = 0;
    if (scopeState.compare_exchange_strong(expected, ok ? 1 : 2) && !ok)
        qInfo("sandbox: no systemd user manager with a memory controller to put the sandbox in a cgroup; memory and "
              "task caps are the per-process rlimits only");
    return ok;
}
} // namespace

bool cgroupLimitsAvailable()
{
    return scopesAvailable();
}

void overrideCgroupLimits(bool available)
{
    scopeState = available ? 1 : 2;
}

void forgetCgroupProbe()
{
    scopeState = 0;
}

void limitInCgroup(QString &program, QStringList &arguments, qint64 memoryBytes, int tasks)
{
    if (program.isEmpty() || !scopesAvailable())
        return;
    QStringList wrapped = scopePrefix();
    if (wrapped.isEmpty())
        return;
    // Swap off: a page pushed out is a page that did not count against the cap.
    wrapped << QStringLiteral("-p") << QStringLiteral("MemoryMax=%1").arg(memoryBytes) << QStringLiteral("-p")
            << QStringLiteral("MemorySwapMax=0") << QStringLiteral("-p") << QStringLiteral("TasksMax=%1").arg(tasks)
            << QStringLiteral("--") << program << arguments;
    program = wrapped.takeFirst();
    arguments = wrapped;
}

QStringList etcBinds(const QStringList &extra)
{
    // Not all of /etc: TeX reads any file it can see, so a shared note's \input{/etc/machine-id} would put the
    // id into the SVG Publish ships. What the tools open there (found by running each under a bare /etc and
    // adding back until it worked): TeX Live's config, fontconfig, the loader's cache, the JVM's security
    // files (the directory is java-<vendor> or java-<n>-<vendor>, and the JVM refuses to start without it),
    // and the alternatives links the system's java and tex binaries may be.
    QStringList names = {QStringLiteral("texmf"), QStringLiteral("fonts"), QStringLiteral("ld.so.cache"),
                         QStringLiteral("alternatives")};
    names << QDir(QStringLiteral("/etc")).entryList({QStringLiteral("java*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    names << extra;
    QStringList arguments;
    for (const QString &name : std::as_const(names)) {
        const QString path = QStringLiteral("/etc/") + name;
        arguments << QStringLiteral("--ro-bind-try") << path << path;
    }
    return arguments;
}

Command bubblewrapCommand(const QString &dir, const QString &program, const QStringList &arguments,
                          const QStringList &environment, const QStringList &readOnlyBinds, qint64 memoryBytes)
{
    Command command;
    command.program = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (command.program.isEmpty())
        return command;
    // A tool outside /usr (a path in a setting, a symlink into a home) is not in the sandbox: what it really is
    // is bound, read-only, and run by that name. Inside /usr it keeps its own: TeX picks its format by the name
    // it is run as (latex -> pdftex).
    QStringList binds = readOnlyBinds;
    QString runPath = program;
    const QString real = QFileInfo(program).canonicalFilePath();
    const auto inUsr = [](const QString &path) { return path.startsWith(QLatin1String("/usr/")); };
    if (!real.isEmpty() && !(inUsr(program) && inUsr(real))) {
        runPath = real;
        if (!inUsr(real) && !binds.contains(real))
            binds << real;
    }
    command.arguments = etcBinds();
    command.arguments << QStringList{QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
                         QStringLiteral("--ro-bind-try"), QStringLiteral("/var/lib/texmf"), QStringLiteral("/var/lib/texmf"),
                         // The system's font cache: without it Graphviz, pango and dvisvgm rebuild one in the
                         // fresh HOME on every first render (150 ms for dot, seconds with many fonts).
                         QStringLiteral("--ro-bind-try"), QStringLiteral("/var/cache/fontconfig"), QStringLiteral("/var/cache/fontconfig"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
                         QStringLiteral("--proc"), QStringLiteral("/proc"),
                         QStringLiteral("--dev"), QStringLiteral("/dev"),
                         // /dev/shm is a tmpfs of half the RAM unless it is sized: the same 64 MiB as /tmp,
                         // mounted over the one --dev made.
                         QStringLiteral("--size"), QStringLiteral("67108864"), QStringLiteral("--tmpfs"), QStringLiteral("/dev/shm"),
                         QStringLiteral("--size"), QStringLiteral("67108864"), QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                         QStringLiteral("--bind"), dir, dir,
                         QStringLiteral("--clearenv"),
                         QStringLiteral("--setenv"), QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"),
                         QStringLiteral("--setenv"), QStringLiteral("HOME"), dir};
#ifdef NOTEAPPC_DEV_ENV_OVERRIDES
    // Tests only: directories (colon-separated) a fake tool may write its books into. The environment
    // variable is not read in a release build.
    for (const QString &path : qEnvironmentVariable("NOTEAPPC_SANDBOX_TEST_BINDS").split(QLatin1Char(':'), Qt::SkipEmptyParts))
        command.arguments << QStringLiteral("--bind") << path << path;
#endif
    for (const QString &path : std::as_const(binds))
        command.arguments << QStringLiteral("--ro-bind") << path << path;
    for (const QString &entry : environment) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        command.arguments << QStringLiteral("--setenv") << entry.left(equals) << entry.mid(equals + 1);
    }
    // --unshare-all is user, ipc, pid, net, uts and cgroup; the user namespace is required here, not
    // best effort, and the processes inside can make no further one.
    command.arguments << QStringLiteral("--unshare-all") << QStringLiteral("--unshare-user")
                      << QStringLiteral("--disable-userns") << QStringLiteral("--cap-drop") << QStringLiteral("ALL")
                      << QStringLiteral("--die-with-parent") << QStringLiteral("--new-session")
                      << QStringLiteral("--chdir") << dir << QStringLiteral("--") << runPath << arguments;
    command.filterFD = limitProcesses(command.arguments, 256) ? addSyscallFilter(command.arguments) : -1;
    if (command.filterFD < 0) { // no filter, no run
        command.program.clear();
        command.arguments.clear();
        return command;
    }
    limitInCgroup(command.program, command.arguments, memoryBytes);
    return command;
}

bool limitProcesses(QStringList &bwrapArguments, int processes)
{
    const qsizetype end = bwrapArguments.indexOf(QStringLiteral("--"));
    if (end < 0 || !QFileInfo::exists(QStringLiteral("/usr/bin/prlimit")))
        return false;
    const QStringList prefix = {QStringLiteral("/usr/bin/prlimit"), QStringLiteral("--nproc=%1").arg(processes), QStringLiteral("--")};
    for (qsizetype i = 0; i < prefix.size(); ++i)
        bwrapArguments.insert(end + 1 + i, prefix.at(i));
    return true;
}

void prepareChild(const Limits &limits, int filterFD)
{
    const auto set = [](int resource, rlim_t value) {
        if (value == 0)
            return;
        const rlimit limit{value, value};
        setrlimit(resource, &limit);
    };
    set(RLIMIT_CPU, limits.cpuSeconds);
    set(RLIMIT_AS, limits.addressSpace);
    set(RLIMIT_DATA, limits.data);
    set(RLIMIT_FSIZE, limits.fileSize);
    set(RLIMIT_NOFILE, limits.openFiles);
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    keepInChild(filterFD);
}

Outcome run(const Command &command, const Limits &limits, const QByteArray &input, int timeoutMs, qint64 maxOutput,
            bool mergeChannels)
{
    Outcome outcome;
    if (command.program.isEmpty()) {
        outcome.failure = QCoreApplication::translate("sandbox", "it runs only inside bubblewrap's sandbox; install bubblewrap");
        return outcome;
    }
    QProcess process;
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    process.setProcessEnvironment(environment);
    if (mergeChannels)
        process.setProcessChannelMode(QProcess::MergedChannels);
    process.setChildProcessModifier([limits, fd = command.filterFD] { prepareChild(limits, fd); });
    process.start(command.program, command.arguments);
    if (command.filterFD >= 0)
        ::close(command.filterFD); // the fork has happened; bwrap holds its own copy
    if (!process.waitForStarted(5000)) {
        outcome.failure = QCoreApplication::translate("sandbox", "could not start the sandbox: %1").arg(process.errorString());
        return outcome;
    }
    process.write(input);
    process.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    const auto drain = [&] {
        outcome.out += process.readAllStandardOutput();
        if (!mergeChannels) // Qt warns when stderr is read from a merged channel
            outcome.err += process.readAllStandardError();
    };
    for (;;) {
        const bool done = process.waitForFinished(20);
        drain();
        if (done)
            break;
        if (outcome.out.size() + outcome.err.size() > maxOutput) {
            outcome.failure = QCoreApplication::translate("sandbox", "more than %1 KiB of output").arg(maxOutput / 1024);
            break;
        }
        if (clock.elapsed() > timeoutMs) {
            outcome.failure = QCoreApplication::translate("sandbox", "timed out after %1 s").arg(timeoutMs / 1000);
            break;
        }
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(2000);
        drain();
    }
    if (outcome.failure.isEmpty() && outcome.out.size() + outcome.err.size() > maxOutput)
        outcome.failure = QCoreApplication::translate("sandbox", "more than %1 KiB of output").arg(maxOutput / 1024);
    if (outcome.failure.isEmpty())
        outcome.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return outcome;
}

} // namespace noteAppC::sandbox
