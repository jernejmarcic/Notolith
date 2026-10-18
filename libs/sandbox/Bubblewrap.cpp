#include "Bubblewrap.h"

#include "SyscallFilter.h"

#include <QStandardPaths>

namespace noteAppC::sandbox {

Command bubblewrapCommand(const QString &dir, const QString &program, const QStringList &arguments,
                          const QStringList &environment, const QStringList &readOnlyBinds)
{
    Command command;
    command.program = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (command.program.isEmpty())
        return command;
    command.arguments = {QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
                         QStringLiteral("--ro-bind"), QStringLiteral("/etc"), QStringLiteral("/etc"),
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
