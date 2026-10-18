#include "Bubblewrap.h"

#include "SyscallFilter.h"

#include <QDir>
#include <QStandardPaths>

namespace noteAppC::sandbox {

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
                          const QStringList &environment, const QStringList &readOnlyBinds)
{
    Command command;
    command.program = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (command.program.isEmpty())
        return command;
    command.arguments = etcBinds();
    command.arguments << QStringList{QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
                         QStringLiteral("--ro-bind-try"), QStringLiteral("/var/lib/texmf"), QStringLiteral("/var/lib/texmf"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
                         QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
                         QStringLiteral("--proc"), QStringLiteral("/proc"),
                         QStringLiteral("--dev"), QStringLiteral("/dev"),
                         QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                         QStringLiteral("--bind"), dir, dir,
                         QStringLiteral("--clearenv"),
                         QStringLiteral("--setenv"), QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"),
                         QStringLiteral("--setenv"), QStringLiteral("HOME"), dir};
    for (const QString &path : readOnlyBinds)
        command.arguments << QStringLiteral("--ro-bind") << path << path;
    for (const QString &entry : environment) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        command.arguments << QStringLiteral("--setenv") << entry.left(equals) << entry.mid(equals + 1);
    }
    command.arguments << QStringLiteral("--unshare-all") << QStringLiteral("--die-with-parent")
                      << QStringLiteral("--new-session") << QStringLiteral("--chdir") << dir << QStringLiteral("--")
                      << program << arguments;
    command.filterFD = addSyscallFilter(command.arguments);
    return command;
}

} // namespace noteAppC::sandbox
