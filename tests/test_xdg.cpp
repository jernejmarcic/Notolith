// SPDX-License-Identifier: GPL-3.0-or-later
// Where Notolith keeps its files: the XDG helper, the migration from the old noteAppC directories, and the
// log file two vault processes share.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>

#include "ConfigFile.h"
#include "MessageLog.h"
#include "Settings.h"
#include "SingleInstance.h"
#include "StorageMigration.h"

#include <QCryptographicHash>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace noteAppC;

namespace {

/// Every variable the helper and the socket names read, put back after the test.
struct Environment {
    static constexpr const char *names[] = {"HOME",           "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                                            "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_DATA_DIRS", "XDG_CONFIG_DIRS",
                                            "TMPDIR",         "NOTEAPPC_INSTANCE_SOCKET"};
    static constexpr int count = sizeof names / sizeof names[0];
    QByteArray saved[count];
    bool wasSet[count];
    Environment()
    {
        for (int i = 0; i < count; ++i) {
            wasSet[i] = qEnvironmentVariableIsSet(names[i]);
            saved[i] = qgetenv(names[i]);
        }
    }
    ~Environment()
    {
        for (int i = 0; i < count; ++i) {
            if (wasSet[i])
                qputenv(names[i], saved[i]);
            else
                qunsetenv(names[i]);
        }
    }
};

void write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    file.write(bytes);
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// The nine permission bits (QFileInfo also reports the same ones a second time as the User flags).
QFileDevice::Permissions mode(const QString &path)
{
    return QFileInfo(path).permissions()
           & (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup
              | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
              | QFileDevice::ExeOther);
}

QString sha1Of(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace

TEST(XDGroot, EachBaseDirectoryIsTheAbsoluteVariableOrTheSpecDefault)
{
    Environment environment;
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    const QString h = QDir::cleanPath(QDir::homePath());
    struct Root {
        XDGroot root;
        const char *variable;
        const char *fallback;
    };
    const Root roots[] = {{XDGroot::Config, "XDG_CONFIG_HOME", "/.config"},
                          {XDGroot::Data, "XDG_DATA_HOME", "/.local/share"},
                          {XDGroot::State, "XDG_STATE_HOME", "/.local/state"},
                          {XDGroot::Cache, "XDG_CACHE_HOME", "/.cache"}};
    struct Case {
        const char *value; // nullptr: unset
        bool useful;       // the variable is taken
        const char *expected;
    };
    const Case cases[] = {{nullptr, false, ""},     {"", false, ""},         {"/srv/x", true, "/srv/x"},
                          {"/srv/x/", true, "/srv/x"}, {"relative/dir", false, ""}, {"./x", false, ""},
                          {"~/x", false, ""}};
    for (const Root &root : roots) {
        for (const Case &c : cases) {
            if (c.value)
                qputenv(root.variable, c.value);
            else
                qunsetenv(root.variable);
            const QString want = c.useful ? QString::fromLatin1(c.expected) : h + QLatin1String(root.fallback);
            EXPECT_EQ(xdgRoot(root.root), want) << root.variable << "=" << (c.value ? c.value : "(unset)");
            EXPECT_EQ(appDir(root.root), want + QStringLiteral("/notolith"));
            EXPECT_EQ(appDir(root.root, QStringLiteral("a/b")), want + QStringLiteral("/notolith/a/b"));
        }
    }
}

TEST(XDGroot, TheRuntimeDirectoryIsTheAbsoluteVariableOrWhatQtMakes)
{
    Environment environment;
    // Qt makes $TMPDIR/runtime-<user> when there is no usable variable: here, never in the machine's /tmp.
    QTemporaryDir tmp;
    qputenv("TMPDIR", tmp.path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", "/run/user/4242/");
    EXPECT_EQ(xdgRoot(XDGroot::Runtime), QStringLiteral("/run/user/4242"));
    EXPECT_EQ(appDir(XDGroot::Runtime), QStringLiteral("/run/user/4242/notolith"));

    // Unset, empty or relative: never a relative answer, and never the relative value itself.
    for (const char *value : {static_cast<const char *>(nullptr), "", "run/here"}) {
        if (value)
            qputenv("XDG_RUNTIME_DIR", value);
        else
            qunsetenv("XDG_RUNTIME_DIR");
        const QString runtime = xdgRoot(XDGroot::Runtime);
        EXPECT_TRUE(runtime.isEmpty() || QDir::isAbsolutePath(runtime)) << (value ? value : "(unset)");
        EXPECT_NE(runtime, QStringLiteral("run/here"));
    }
}

TEST(XDGroot, SearchDirectoriesStartWithTheUsersAndKeepOnlyAbsoluteEntries)
{
    Environment environment;
    qputenv("XDG_DATA_HOME", "/home/u/data");
    qunsetenv("XDG_DATA_DIRS");
    EXPECT_EQ(xdgSearchDirs(XDGroot::Data),
              (QStringList{QStringLiteral("/home/u/data"), QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share")}));
    qputenv("XDG_DATA_DIRS", "");
    EXPECT_EQ(xdgSearchDirs(XDGroot::Data).size(), 3) << "empty means the default";
    qputenv("XDG_DATA_DIRS", "/opt/a:relative:/opt/b/::/opt/c");
    EXPECT_EQ(xdgSearchDirs(XDGroot::Data),
              (QStringList{QStringLiteral("/home/u/data"), QStringLiteral("/opt/a"), QStringLiteral("/opt/b"),
                           QStringLiteral("/opt/c")}));

    qputenv("XDG_CONFIG_HOME", "/home/u/config");
    qunsetenv("XDG_CONFIG_DIRS");
    EXPECT_EQ(xdgSearchDirs(XDGroot::Config), (QStringList{QStringLiteral("/home/u/config"), QStringLiteral("/etc/xdg")}));
}

TEST(XDGroot, AVaultHasOneKeyHoweverItsPathIsSpelled)
{
    const QString key = vaultDigest(QStringLiteral("/vaults/mine"));
    EXPECT_EQ(key.size(), 16);
    EXPECT_EQ(key, vaultDigest(QStringLiteral("/vaults/mine/")));
    EXPECT_NE(key, vaultDigest(QStringLiteral("/vaults/other")));
}

TEST(XDGroot, EnsurePrivateDirMakesEveryMissingDirectoryOwnerOnly)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QFileDevice::Permissions ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    const QString leaf = root.filePath(QStringLiteral("a/b/c"));
    ASSERT_TRUE(ensurePrivateDir(leaf));
    for (const char *part : {"a", "a/b", "a/b/c"})
        EXPECT_EQ(mode(root.filePath(QString::fromLatin1(part))), ownerOnly) << part;

    // A directory that is already there keeps what it has.
    QFile::setPermissions(leaf, ownerOnly | QFileDevice::ReadGroup | QFileDevice::ExeGroup);
    ASSERT_TRUE(ensurePrivateDir(leaf));
    EXPECT_TRUE(QFileInfo(leaf).permissions().testFlag(QFileDevice::ReadGroup));
    EXPECT_TRUE(ensurePrivateDir(leaf + QStringLiteral("/")));
}

namespace {

/// A machine whose four XDG homes hold what an older build left, and nothing else.
struct OldLayout {
    Environment environment;
    QTemporaryDir root;
    QTemporaryDir vault;
    QString config, data, state, cache;
    QString vaultPath = QDir(vault.path()).absolutePath();
    QString digest = vaultDigest(vaultPath);
    QString oldKey = sha1Of(vaultPath);
    QString strangerKey = sha1Of(QStringLiteral("/a/vault/nobody/remembers"));

    OldLayout()
    {
        config = root.filePath(QStringLiteral("config"));
        data = root.filePath(QStringLiteral("data"));
        state = root.filePath(QStringLiteral("state"));
        cache = root.filePath(QStringLiteral("cache"));
        qputenv("HOME", root.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", config.toUtf8());
        qputenv("XDG_DATA_HOME", data.toUtf8());
        qputenv("XDG_STATE_HOME", state.toUtf8());
        qputenv("XDG_CACHE_HOME", cache.toUtf8());
        qputenv("XDG_RUNTIME_DIR", root.filePath(QStringLiteral("run")).toUtf8()); // the migration lock lives here
        qunsetenv("XDG_DATA_DIRS");
    }

    void populate()
    {
        // What the app remembered: written by the current Settings, then moved to where the old build kept it.
        {
            Settings settings = Settings::load();
            settings.noteOpened(vaultPath, vaultPath + QStringLiteral("/Note.md"));
            ASSERT_TRUE(settings.save());
            ASSERT_TRUE(QDir().mkpath(config + QStringLiteral("/noteAppC")));
            ASSERT_TRUE(QFile::rename(Settings::indexFile(), config + QStringLiteral("/noteAppC/vaults.ini")));
            QFile::remove(Settings::configFile());
            QDir(config + QStringLiteral("/notolith")).removeRecursively();
            QDir(state + QStringLiteral("/notolith")).removeRecursively();
        }
        const QString oldConfig = config + QStringLiteral("/noteAppC/");
        write(oldConfig + QStringLiteral("settings.ini"), "[editor]\ntab-width = 2\n");
        write(oldConfig + QStringLiteral("themes/Mine.json"), "{}");
        write(oldConfig + QStringLiteral("hotkeys.json"), "{}");
        write(oldConfig + QStringLiteral("plugins/ai/ai.ini"), "[ai]\nprovider = ollama\n");
        write(oldConfig + QStringLiteral("epub-positions.ini"), "[book_x]\nspineIndex=3\n");
        write(oldConfig + QStringLiteral(".save.lock"), "123\n");
        for (const char *name : {"bookmarks.json", "icons.json", "workspaces.json", "workspace.json"})
            write(oldConfig + QStringLiteral("vaults/") + digest + QLatin1Char('/') + QString::fromLatin1(name),
                  QByteArray("{\"is\":\"") + name + "\"}");
        write(oldConfig + QStringLiteral("vaults/") + oldKey + QStringLiteral("/graph.json"), "{\"graph\":1}");
        write(oldConfig + QStringLiteral("vaults/") + strangerKey + QStringLiteral("/graph.json"), "{\"graph\":2}");
        {
            QSettings window(oldConfig + QStringLiteral("noteAppC.conf"), QSettings::IniFormat);
            window.setValue(QStringLiteral("window/geometry"), QByteArray("geometry-bytes"));
            window.setValue(QStringLiteral("window/rightSidebar"), false);
            window.setValue(QStringLiteral("publish/") + oldKey + QStringLiteral("/folder"), QStringLiteral("Public"));
            window.setValue(QStringLiteral("publish/") + strangerKey + QStringLiteral("/folder"), QStringLiteral("Lost"));
        }
        const QString oldData = data + QStringLiteral("/noteAppC/");
        write(oldData + QStringLiteral("recovery/n1/path"), "/x/Note.md");
        write(oldData + QStringLiteral("history/abc.git/HEAD"), "ref: refs/heads/main\n");
        write(oldData + QStringLiteral("sync/") + digest + QStringLiteral("/journal.db"), "db");
        write(oldData + QStringLiteral("plugins/p/plugin.ini"), "[plugin]\n");
        write(oldData + QStringLiteral("share/link-0123.lock"), "1\n");
        write(oldData + QStringLiteral("share/link.log"), "old log\n");
        write(state + QStringLiteral("/noteAppC/link.log"), "link\n");
        write(cache + QStringLiteral("/noteAppC/noteAppC/vaults/") + digest + QStringLiteral("/index.db"), "index");
        write(cache + QStringLiteral("/noteAppC/icons/x.svg"), "<svg/>");
    }
};

} // namespace

TEST(StorageMigration, EveryFileLandsWhereTheSpecPutsItAndTheOldDirectoriesGo)
{
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    const QString c = machine.config + QStringLiteral("/notolith/");
    const QString d = machine.data + QStringLiteral("/notolith/");
    const QString s = machine.state + QStringLiteral("/notolith/");

    EXPECT_GT(migrateStorage(), 10);

    // Configuration stays configuration.
    EXPECT_EQ(read(c + QStringLiteral("settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_TRUE(QFileInfo::exists(c + QStringLiteral("themes/Mine.json")));
    EXPECT_TRUE(QFileInfo::exists(c + QStringLiteral("hotkeys.json")));
    EXPECT_TRUE(QFileInfo::exists(c + QStringLiteral("plugins/ai/ai.ini")));
    // What the app remembers is state.
    EXPECT_TRUE(QFileInfo::exists(s + QStringLiteral("vaults.ini")));
    EXPECT_TRUE(QFileInfo::exists(s + QStringLiteral("epub-positions.ini")));
    EXPECT_TRUE(QFileInfo::exists(s + QStringLiteral("vaults/") + machine.digest + QStringLiteral("/workspace.json")));
    EXPECT_EQ(read(s + QStringLiteral("vaults/") + machine.digest + QStringLiteral("/graph.json")), QByteArray("{\"graph\":1}"))
        << "re-keyed by the recent-vaults list";
    EXPECT_EQ(read(s + QStringLiteral("vaults/") + machine.strangerKey + QStringLiteral("/graph.json")), QByteArray("{\"graph\":2}"))
        << "a vault nobody remembers keeps its file under the key it had";
    EXPECT_TRUE(QFileInfo::exists(s + QStringLiteral("link.log")));
    // What a person made is data.
    for (const char *name : {"bookmarks.json", "icons.json", "workspaces.json"})
        EXPECT_TRUE(QFileInfo::exists(d + QStringLiteral("vaults/") + machine.digest + QLatin1Char('/') + QString::fromLatin1(name))) << name;
    EXPECT_TRUE(QFileInfo::exists(d + QStringLiteral("recovery/n1/path")));
    EXPECT_TRUE(QFileInfo::exists(d + QStringLiteral("history/abc.git/HEAD")));
    EXPECT_TRUE(QFileInfo::exists(d + QStringLiteral("sync/") + machine.digest + QStringLiteral("/journal.db")));
    EXPECT_TRUE(QFileInfo::exists(d + QStringLiteral("plugins/p/plugin.ini")));
    // A lock and a log of a process are not kept.
    EXPECT_FALSE(QFileInfo::exists(d + QStringLiteral("share/link-0123.lock")));
    EXPECT_FALSE(QFileInfo::exists(d + QStringLiteral("share/link.log")));
    EXPECT_FALSE(QFileInfo::exists(c + QStringLiteral(".save.lock")));

    // The window state is copied into window.ini, the publish choices under the new key.
    const QSettings window(s + QStringLiteral("window.ini"), QSettings::IniFormat);
    EXPECT_EQ(window.value(QStringLiteral("window/geometry")).toByteArray(), QByteArray("geometry-bytes"));
    EXPECT_FALSE(window.value(QStringLiteral("window/rightSidebar")).toBool());
    EXPECT_EQ(window.value(QStringLiteral("publish/") + machine.digest + QStringLiteral("/folder")).toString(),
              QStringLiteral("Public"));
    EXPECT_EQ(window.value(QStringLiteral("publish/") + machine.strangerKey + QStringLiteral("/folder")).toString(),
              QStringLiteral("Lost"));
    EXPECT_FALSE(window.contains(QStringLiteral("publish/") + machine.oldKey + QStringLiteral("/folder")));

    // The recent vaults came along, and the cache is gone rather than moved.
    EXPECT_EQ(Settings::load().recentVaults().value(0), machine.vaultPath);
    EXPECT_FALSE(QFileInfo::exists(machine.cache + QStringLiteral("/noteAppC")));
    EXPECT_FALSE(QFileInfo::exists(machine.cache + QStringLiteral("/notolith")));
    // Nothing is left behind in the three old directories.
    for (const QString &old : {machine.config, machine.data, machine.state})
        EXPECT_FALSE(QFileInfo::exists(old + QStringLiteral("/noteAppC"))) << old.toStdString();
    // The directories it made are the user's alone.
    EXPECT_EQ(mode(s), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
}

TEST(StorageMigration, ARunThatHasHappenedIsNotRepeatedAndAFreshMachineIsLeftAlone)
{
    {
        OldLayout fresh;
        EXPECT_EQ(migrateStorage(), 0);
        EXPECT_FALSE(QFileInfo::exists(fresh.state + QStringLiteral("/notolith"))) << "nothing to migrate, nothing made";
    }
    OldLayout machine;
    machine.populate();
    ASSERT_GT(migrateStorage(), 0);
    // An older window that was still open writes its old file again: the marker keeps it where it is.
    write(machine.config + QStringLiteral("/noteAppC/hotkeys.json"), "{\"again\":1}");
    EXPECT_EQ(migrateStorage(), 0);
    EXPECT_TRUE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC/hotkeys.json")));
    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/hotkeys.json")), QByteArray("{}"));
}

TEST(StorageMigration, ADestinationThatExistsIsNeverOverwritten)
{
    OldLayout machine;
    machine.populate();
    write(machine.config + QStringLiteral("/notolith/settings.ini"), "[editor]\ntab-width = 8\n");
    write(machine.data + QStringLiteral("/notolith/recovery/n1/path"), "/mine/Note.md");

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    // Said once per file, and once more in all.
    EXPECT_EQ(warnings.filter(QStringLiteral("settings.ini")).size(), 1);
    EXPECT_EQ(warnings.filter(QStringLiteral("already exists")).size(), 2);
    EXPECT_EQ(warnings.size(), 3);
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None) << "nothing a later start could change: it is not tried again";
    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/settings.ini")), QByteArray("[editor]\ntab-width = 8\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/notolith/recovery/n1/path")), QByteArray("/mine/Note.md"));
    // What could not move stays where it was, for the person to look at.
    EXPECT_EQ(read(machine.config + QStringLiteral("/noteAppC/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/noteAppC/recovery/n1/path")), QByteArray("/x/Note.md"));
    // Everything else did move.
    EXPECT_TRUE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/themes/Mine.json")));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC/themes")));
    // The exact paths are on record, once, for the person to act on.
    const QByteArray marker = read(machine.state + QStringLiteral("/notolith/.migrated"));
    EXPECT_TRUE(marker.startsWith("attention")) << marker.toStdString();
    EXPECT_TRUE(marker.contains((machine.config + QStringLiteral("/noteAppC/settings.ini\t")).toUtf8()));
    EXPECT_TRUE(marker.contains((machine.data + QStringLiteral("/noteAppC/recovery/n1/path\t")).toUtf8()));
    // Each file says where its new place is, and the summary says what to do about it.
    const QString newSettings = machine.config + QStringLiteral("/notolith/settings.ini");
    EXPECT_TRUE(warnings.filter(QStringLiteral("settings.ini")).first().contains(QStringLiteral("new place: ") + newSettings));
    EXPECT_TRUE(marker.contains((QStringLiteral("(new place: ") + newSettings + QLatin1Char(')')).toUtf8()));
    const QString marked = machine.state + QStringLiteral("/notolith/.migrated");
    EXPECT_TRUE(warnings.last().contains(QStringLiteral("delete ") + marked)) << warnings.last().toStdString();

    // The next start has nothing to do and nothing to say, though the clash is still there.
    warnings.clear();
    EXPECT_EQ(migrateStorage(nullptr, &warnings), 0);
    EXPECT_TRUE(warnings.isEmpty());
    EXPECT_EQ(read(machine.config + QStringLiteral("/noteAppC/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));

    // Deleting the marker makes the next start look again: a clash that is still there is said again, and the
    // person who put the old file in its new place has it taken as moved.
    ASSERT_TRUE(QFile::remove(marked));
    ASSERT_TRUE(QFile::remove(newSettings));
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Fresh);
    EXPECT_EQ(migrateStorage(nullptr, &warnings), 1) << "settings.ini now has its new place free";
    EXPECT_EQ(warnings.filter(QStringLiteral("recovery/n1/path")).size(), 1) << "still in the way, said again";
    EXPECT_EQ(read(newSettings), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/notolith/recovery/n1/path")), QByteArray("/mine/Note.md"));
}

TEST(StorageMigration, TwoStartsAtTheSameInstantMoveEachFileOnceAndSayNothingOfTheOther)
{
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    constexpr int many = 1500;
    for (int i = 0; i < many; ++i)
        write(machine.config + QStringLiteral("/noteAppC/themes/t%1.json").arg(i), QByteArray::number(i));

    // Two processes, let go together: each writes what it moved and said to a file of its own.
    int gate[2];
    ASSERT_EQ(::pipe(gate), 0);
    pid_t children[2];
    for (int n = 0; n < 2; ++n) {
        children[n] = ::fork();
        ASSERT_GE(children[n], 0);
        if (children[n] == 0) {
            ::close(gate[1]);
            char go;
            if (::read(gate[0], &go, 1) != 0)
                ::_exit(2); // the parent closes the pipe without writing: end of file is the signal
            QStringList lines, warnings;
            const int moved = migrateStorage(&lines, &warnings);
            QFile out(machine.root.filePath(QStringLiteral("out%1").arg(n)));
            if (!out.open(QIODevice::WriteOnly))
                ::_exit(3);
            out.write(QByteArray::number(moved) + '\n' + warnings.join(QLatin1Char('\n')).toUtf8());
            out.close(); // _exit does not run destructors
            ::_exit(0);
        }
    }
    ::close(gate[0]);
    ::close(gate[1]);
    for (pid_t child : children) {
        int status = 0;
        ASSERT_EQ(::waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    }

    const QList<QByteArray> first = read(machine.root.filePath(QStringLiteral("out0"))).split('\n');
    const QList<QByteArray> second = read(machine.root.filePath(QStringLiteral("out1"))).split('\n');
    const int movedFirst = first.value(0).toInt(), movedSecond = second.value(0).toInt();
    EXPECT_TRUE((movedFirst > many) != (movedSecond > many)) << "one did everything: " << movedFirst << " and " << movedSecond;
    EXPECT_EQ(movedFirst + movedSecond, movedFirst > movedSecond ? movedFirst : movedSecond) << "the other did nothing";
    EXPECT_TRUE(first.value(1).isEmpty() && second.value(1).isEmpty()) << "no refusal, no phantom: " << first.value(1).toStdString()
                                                                       << second.value(1).toStdString();
    EXPECT_TRUE(read(machine.state + QStringLiteral("/notolith/.migrated")).startsWith("complete"));
    EXPECT_EQ(read(machine.state + QStringLiteral("/notolith/.migrated")).count('\n'), 1) << "no path on it";
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC")));
    for (int i = 0; i < many; ++i)
        ASSERT_EQ(read(machine.config + QStringLiteral("/notolith/themes/t%1.json").arg(i)), QByteArray::number(i)) << i;
}

TEST(StorageMigration, AnEmptyLockLeftByACrashDoesNotHoldTheFilesBackForEver)
{
    OldLayout machine;
    machine.populate();
    // A start that died between making the lock file and writing its pid into it, long ago.
    const QString lockDir = machine.root.filePath(QStringLiteral("run/notolith"));
    ASSERT_TRUE(QDir().mkpath(lockDir));
    ASSERT_TRUE(QFile::setPermissions(lockDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QFile lock(lockDir + QStringLiteral("/migrate.lock"));
    ASSERT_TRUE(lock.open(QIODevice::WriteOnly));
    ASSERT_TRUE(lock.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    lock.close();

    QStringList warnings;
    EXPECT_GT(migrateStorage(nullptr, &warnings), 0);
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
}

TEST(StorageMigration, AFileInTheWayIsSaidOnceWhileOneThatMayStillMoveIsTriedAgain)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "a directory closed to the owner is open to root";
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    write(machine.config + QStringLiteral("/notolith/settings.ini"), "[editor]\ntab-width = 8\n");
    // The data directory refuses writes for this one start: those files may move at the next.
    const QString data = machine.data + QStringLiteral("/notolith");
    ASSERT_TRUE(QDir().mkpath(data));
    ASSERT_TRUE(QFile::setPermissions(data, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QStringList warnings;
    migrateStorage(nullptr, &warnings);
    EXPECT_EQ(warnings.filter(QStringLiteral("settings.ini")).size(), 1);
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Retry);
    const QByteArray marker = read(machine.state + QStringLiteral("/notolith/.migrated"));
    EXPECT_TRUE(marker.startsWith("incomplete")) << marker.toStdString();
    EXPECT_TRUE(marker.contains((machine.config + QStringLiteral("/noteAppC/settings.ini\t")).toUtf8()))
        << "what is in the way is kept on record while the rest is retried";

    ASSERT_TRUE(QFile::setPermissions(data, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    warnings.clear();
    EXPECT_GT(migrateStorage(nullptr, &warnings), 0);
    EXPECT_TRUE(warnings.filter(QStringLiteral("settings.ini")).isEmpty()) << "said once, not at every start";
    EXPECT_TRUE(QFileInfo::exists(data + QStringLiteral("/recovery/n1/path")));
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
    EXPECT_TRUE(read(machine.state + QStringLiteral("/notolith/.migrated")).contains("settings.ini\t"))
        << "still on record after the retry finished";
    EXPECT_EQ(read(machine.config + QStringLiteral("/noteAppC/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
}

TEST(StorageMigration, ACopyInPlaceWhoseOriginalCannotGoIsSaidNotCalledMoved)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "a directory closed to the owner is open to root";
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    // A directory of the old tree this user cannot write (left by a run under sudo): its files can be copied, not removed.
    const QString closed = machine.data + QStringLiteral("/noteAppC/recovery/n1");
    ASSERT_TRUE(QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QStringList lines, warnings;
    migrateStorage(&lines, &warnings);
    ASSERT_TRUE(QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    EXPECT_EQ(read(machine.data + QStringLiteral("/notolith/recovery/n1/path")), QByteArray("/x/Note.md"));
    EXPECT_TRUE(QFileInfo::exists(closed + QStringLiteral("/path"))) << "the original is still there";
    const QStringList said = warnings.filter(QStringLiteral("recovery/n1/path"));
    ASSERT_EQ(said.size(), 1);
    EXPECT_TRUE(said.first().contains(QStringLiteral("original could not be removed"))) << said.first().toStdString();
    EXPECT_TRUE(read(machine.state + QStringLiteral("/notolith/.migrated")).startsWith("attention"));
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
}

TEST(MessageLog, AProcessThatStartsWhileAnotherIsWritingDoesNotTakeItsLogAway)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notolith.log"));
    const QString lock = dir.filePath(QStringLiteral("notolith.log.lock"));
    write(path, "from the run before\n");
    {
        MessageLog first(path, lock);
        ASSERT_TRUE(first.isOpen());
        EXPECT_EQ(read(path + QStringLiteral(".old")), QByteArray("from the run before\n")) << "nobody else was writing";
        first.append(QStringLiteral("first one\n"));
        {
            MessageLog second(path, lock); // another vault, another process
            ASSERT_TRUE(second.isOpen());
            second.append(QStringLiteral("second one\n"));
            first.append(QStringLiteral("first two\n"));
        }
        EXPECT_EQ(read(path), QByteArray("first one\nsecond one\nfirst two\n"));
        EXPECT_EQ(read(path + QStringLiteral(".old")), QByteArray("from the run before\n"));
    }
    // Both gone: the next start begins a fresh log and keeps this one as .old.
    MessageLog third(path, lock);
    EXPECT_EQ(read(path + QStringLiteral(".old")), QByteArray("first one\nsecond one\nfirst two\n"));
    EXPECT_TRUE(read(path).isEmpty());
}

TEST(MessageLog, TheFileMovesToOldPastTheCapAndEveryProcessFollowsIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notolith.log"));
    const QString lock = dir.filePath(QStringLiteral("notolith.log.lock"));
    MessageLog first(path, lock, 50);
    MessageLog second(path, lock, 50);
    const QString line = QStringLiteral("0123456789012345678901234567890123456789\n"); // 41 bytes
    first.append(line);
    first.append(line);  // now past 50: the next one rotates
    first.append(QStringLiteral("after\n"));
    EXPECT_EQ(read(path), QByteArray("after\n"));
    EXPECT_EQ(read(path + QStringLiteral(".old")).size(), 82);
    second.append(QStringLiteral("other\n")); // its file is the one moved away: it follows, and does not rotate again
    EXPECT_EQ(read(path), QByteArray("after\nother\n"));
    EXPECT_EQ(read(path + QStringLiteral(".old")).size(), 82);
}

namespace {

/// A directory on another file system than `like`, so that a move between them is a copy; empty when the
/// machine has no second one at hand (/dev/shm is a tmpfs of its own on most).
QString otherFilesystem(const QString &like, QTemporaryDir &holder)
{
    struct stat a, b;
    if (::stat(like.toLocal8Bit().constData(), &a) != 0 || !QFileInfo(QStringLiteral("/dev/shm")).isWritable()
        || !holder.isValid())
        return {};
    return ::stat(holder.path().toLocal8Bit().constData(), &b) == 0 && a.st_dev != b.st_dev ? holder.path() : QString();
}

} // namespace

TEST(StorageMigration, FilesThatCannotMoveStayInTheOldPlaceAndTheNextStartMovesThem)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "a directory closed to the owner is open to root";
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    const QString oldConfig = machine.config + QStringLiteral("/noteAppC/");
    // The state directory refuses writes for this one start (a full disk, a mount that is not there yet).
    const QString state = machine.state + QStringLiteral("/notolith");
    ASSERT_TRUE(QDir().mkpath(state));
    ASSERT_TRUE(QFile::setPermissions(state, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    // The two files with a single place that is not the config directory: nowhere else, nowhere wrong.
    EXPECT_TRUE(QFileInfo::exists(oldConfig + QStringLiteral("vaults.ini")));
    EXPECT_TRUE(QFileInfo::exists(oldConfig + QStringLiteral("noteAppC.conf")));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/vaults.ini")));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/noteAppC.conf")));
    EXPECT_FALSE(warnings.filter(QStringLiteral("vaults.ini")).isEmpty());
    EXPECT_FALSE(warnings.filter(QStringLiteral("noteAppC.conf")).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(state + QStringLiteral("/.migrated"))) << "nothing says it is done";
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Fresh) << "so the next start does it all again";

    ASSERT_TRUE(QFile::setPermissions(state, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    warnings.clear();
    migrateStorage(nullptr, &warnings);
    EXPECT_TRUE(warnings.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(state + QStringLiteral("/vaults.ini")));
    EXPECT_TRUE(QFileInfo::exists(state + QStringLiteral("/window.ini")));
    EXPECT_EQ(read(state + QStringLiteral("/vaults/") + machine.digest + QStringLiteral("/graph.json")),
              QByteArray("{\"graph\":1}"))
        << "re-keyed once the recent vaults are there";
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC")));
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
}

TEST(StorageMigration, AnIndexAlreadyInTheNewPlaceCostsNeitherTheOldOneNorAGraph)
{
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    // The new build ran once before the old directory showed up (a restored backup, a synced folder).
    write(machine.state + QStringLiteral("/notolith/vaults.ini"), "[state]\nrecent-vaults = /elsewhere\n");

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    EXPECT_TRUE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC/vaults.ini"))) << "kept, not moved to a place nothing reads";
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/vaults.ini")));
    EXPECT_FALSE(warnings.filter(QStringLiteral("vaults.ini")).isEmpty());
    // The vaults of the old index still key the graph and the publish choices.
    EXPECT_EQ(read(machine.state + QStringLiteral("/notolith/vaults/") + machine.digest + QStringLiteral("/graph.json")),
              QByteArray("{\"graph\":1}"));
    const QSettings window(machine.state + QStringLiteral("/notolith/window.ini"), QSettings::IniFormat);
    EXPECT_EQ(window.value(QStringLiteral("publish/") + machine.digest + QStringLiteral("/folder")).toString(),
              QStringLiteral("Public"));
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None) << "the clash is told once, not retried";
}

TEST(StorageMigration, ALinkMovesAsALinkWhateverTheDepthOfTheNewRoot)
{
    OldLayout machine;
    machine.data = machine.root.filePath(QStringLiteral("deeper/than/config/data")); // a relative link means something else there
    qputenv("XDG_DATA_HOME", machine.data.toUtf8());
    const QString secret = machine.root.filePath(QStringLiteral("outside/secret.json"));
    write(secret, "{\"secret\":1}");
    const QString oldLink = machine.config + QStringLiteral("/noteAppC/vaults/") + machine.digest + QStringLiteral("/icons.json");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(oldLink).absolutePath()));
    ASSERT_EQ(::symlink("../../../../outside/secret.json", oldLink.toLocal8Bit().constData()), 0);
    ASSERT_EQ(read(oldLink), QByteArray("{\"secret\":1}"));

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    const QString moved = machine.data + QStringLiteral("/notolith/vaults/") + machine.digest + QStringLiteral("/icons.json");
    EXPECT_TRUE(QFileInfo(moved).isSymLink());
    EXPECT_EQ(read(moved), QByteArray("{\"secret\":1}")) << "still points at what it pointed at";
    EXPECT_EQ(read(secret), QByteArray("{\"secret\":1}"));
    EXPECT_TRUE(warnings.isEmpty());
}

TEST(StorageMigration, AcrossFileSystemsALinkIsNotFollowedAndAFileIsCompleteBeforeTheOriginalGoes)
{
    OldLayout machine;
    QTemporaryDir fast(QStringLiteral("/dev/shm/nac-xdg-XXXXXX"));
    const QString other = otherFilesystem(machine.root.path(), fast);
    if (other.isEmpty())
        GTEST_SKIP() << "no second file system";
    machine.state = other + QStringLiteral("/state");
    machine.data = other + QStringLiteral("/data");
    qputenv("XDG_STATE_HOME", machine.state.toUtf8());
    qputenv("XDG_DATA_HOME", machine.data.toUtf8());
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    const QString secret = machine.root.filePath(QStringLiteral("outside/secret.json"));
    write(secret, "{\"secret\":1}");
    const QString vaultDir = machine.config + QStringLiteral("/noteAppC/vaults/") + machine.digest + QLatin1Char('/');
    ASSERT_TRUE(QFile::remove(vaultDir + QStringLiteral("icons.json")));
    ASSERT_TRUE(QFile::link(secret, vaultDir + QStringLiteral("icons.json")));

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    const QString dataVault = machine.data + QStringLiteral("/notolith/vaults/") + machine.digest + QLatin1Char('/');
    EXPECT_TRUE(QFileInfo(dataVault + QStringLiteral("icons.json")).isSymLink()) << "not a second copy of what it points at";
    EXPECT_EQ(QFileInfo(dataVault + QStringLiteral("icons.json")).symLinkTarget(), secret);
    EXPECT_EQ(read(secret), QByteArray("{\"secret\":1}"));
    EXPECT_EQ(read(dataVault + QStringLiteral("workspaces.json")), QByteArray("{\"is\":\"workspaces.json\"}"));
    EXPECT_FALSE(QFileInfo::exists(vaultDir + QStringLiteral("workspaces.json")));
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
    for (const QString &root : {machine.state, machine.data}) {
        QDirIterator all(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (all.hasNext())
            EXPECT_FALSE(all.next().endsWith(QLatin1String(".migrating"))) << all.filePath().toStdString();
    }
}

TEST(StorageMigration, ANamedPipeIsLeftAloneAndSaid)
{
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    const QString pipe = machine.config + QStringLiteral("/noteAppC/vaults/") + machine.digest + QStringLiteral("/pipe.json");
    ASSERT_EQ(::mkfifo(pipe.toLocal8Bit().constData(), 0600), 0);

    QStringList warnings;
    migrateStorage(nullptr, &warnings);

    EXPECT_TRUE(QFileInfo::exists(pipe));
    EXPECT_FALSE(QFileInfo::exists(machine.data + QStringLiteral("/notolith/vaults/") + machine.digest + QStringLiteral("/pipe.json")));
    EXPECT_EQ(warnings.filter(QStringLiteral("not a regular file")).size(), 1);
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None) << "a start cannot make it a file: said once";
}

TEST(StorageMigration, ADirectoryThatIsALinkIsNotEmptiedIntoTheNewOne)
{
    OldLayout machine;
    const QString synced = machine.root.filePath(QStringLiteral("synced/noteAppC"));
    write(synced + QStringLiteral("/hotkeys.json"), "{}");
    ASSERT_TRUE(QDir().mkpath(machine.config));
    ASSERT_TRUE(QFile::link(synced, machine.config + QStringLiteral("/noteAppC")));

    QStringList warnings;
    EXPECT_EQ(migrateStorage(nullptr, &warnings), 0);

    EXPECT_TRUE(QFileInfo::exists(synced + QStringLiteral("/hotkeys.json")));
    EXPECT_TRUE(QFileInfo(machine.config + QStringLiteral("/noteAppC")).isSymLink());
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/hotkeys.json")));
    EXPECT_FALSE(warnings.filter(QStringLiteral("symbolic link")).isEmpty());
    // Nothing a later start could do about it, so it does not keep coming back.
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
    warnings.clear();
    EXPECT_EQ(migrateStorage(nullptr, &warnings), 0);
    EXPECT_TRUE(warnings.isEmpty());
}

TEST(StorageMigration, NothingOlderHereMeansNothingToDo)
{
    OldLayout fresh;
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
    OldLayout machine;
    machine.populate();
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Fresh);
}

TEST(XDGroot, AnEmptyPathIsNotADirectoryToMake)
{
    EXPECT_FALSE(ensurePrivateDir(QString()));
    QTemporaryDir dir;
    const QString own = dir.filePath(QStringLiteral("own"));
    ASSERT_TRUE(ensurePrivateDir(own));
    EXPECT_TRUE(isOwnPrivateDir(own));
    const QString open = dir.filePath(QStringLiteral("open"));
    ASSERT_TRUE(QDir().mkdir(open));
    ASSERT_TRUE(QFile::setPermissions(open, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                | QFileDevice::ReadOther | QFileDevice::ExeOther));
    EXPECT_FALSE(isOwnPrivateDir(open));
    const QString link = dir.filePath(QStringLiteral("link"));
    ASSERT_TRUE(QFile::link(own, link));
    EXPECT_FALSE(isOwnPrivateDir(link)) << "a link is not the directory itself";
    EXPECT_FALSE(isOwnPrivateDir(QString()));
    EXPECT_FALSE(isOwnPrivateDir(dir.filePath(QStringLiteral("missing"))));
}

TEST(SingleInstance, TheSocketDirectoryIsTheUsersOwnAndClosedToOthersOrItIsTheStateDirectory)
{
    Environment environment;
    QTemporaryDir home;
    qunsetenv("NOTEAPPC_INSTANCE_SOCKET");
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_STATE_HOME", home.filePath(QStringLiteral("state")).toUtf8());
    const QString run = home.filePath(QStringLiteral("run"));
    ASSERT_TRUE(QDir().mkpath(run));
    qputenv("XDG_RUNTIME_DIR", run.toUtf8());
    const QString state = home.filePath(QStringLiteral("state/notolith/instance"));

    EXPECT_EQ(SingleInstance::baseName(), run + QStringLiteral("/notolith/instance"));
    EXPECT_TRUE(isOwnPrivateDir(run + QStringLiteral("/notolith")));

    // Someone else's arrangement of the name: a link to a directory, a directory open to all.
    ASSERT_TRUE(QDir(run + QStringLiteral("/notolith")).removeRecursively());
    const QString elsewhere = home.filePath(QStringLiteral("elsewhere"));
    ASSERT_TRUE(ensurePrivateDir(elsewhere));
    ASSERT_TRUE(QFile::link(elsewhere, run + QStringLiteral("/notolith")));
    EXPECT_EQ(SingleInstance::baseName(), state);
    ASSERT_TRUE(QFile::remove(run + QStringLiteral("/notolith")));
    ASSERT_TRUE(QDir().mkdir(run + QStringLiteral("/notolith")));
    ASSERT_TRUE(QFile::setPermissions(run + QStringLiteral("/notolith"), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                                              | QFileDevice::ExeOwner | QFileDevice::WriteOther
                                                                              | QFileDevice::ReadOther | QFileDevice::ExeOther));
    EXPECT_EQ(SingleInstance::baseName(), state);
    EXPECT_TRUE(isOwnPrivateDir(home.filePath(QStringLiteral("state/notolith")))) << "the fallback is made private too";
}

TEST(SingleInstance, AnOpenWindowOfTheBuildBeforeIsFoundAndGivenTheLaunch)
{
    int argc = 1;
    char name[] = "test_xdg";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    Environment environment;
    QTemporaryDir run;
    QTemporaryDir vaultA, vaultB;
    ASSERT_TRUE(QDir(vaultA.path()).mkpath(QStringLiteral(".obsidian")));
    ASSERT_TRUE(QDir(vaultB.path()).mkpath(QStringLiteral(".obsidian")));
    qunsetenv("NOTEAPPC_INSTANCE_SOCKET");
    qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());
    // The recent vaults decide where a bare launch goes: the test's own, not what other tests left in the shared home.
    QTemporaryDir home;
    qputenv("XDG_STATE_HOME", home.filePath(QStringLiteral("state")).toUtf8());
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    const QString id = SingleInstance::nameForVault(vaultA.path()).section(QLatin1String("instance-"), 1);
    const QString oldName = run.path() + QStringLiteral("/notolith-") + id; // where the build before listened
    EXPECT_TRUE(SingleInstance::previousBuildSockets().isEmpty());

    QLockFile held(oldName + QStringLiteral(".lock")); // what that build held while its window ran
    held.setStaleLockTime(0);
    ASSERT_TRUE(held.tryLock());
    QLocalServer window;
    ASSERT_TRUE(window.listen(oldName));
    EXPECT_EQ(SingleInstance::previousBuildSockets(), QStringList{oldName});

    // Another vault has no such window: nothing is delivered, and the caller refuses the launch.
    EXPECT_FALSE(SingleInstance::handOverToPreviousBuild(SingleInstance::launchFor(vaultB.path()), vaultB.path()));
    EXPECT_TRUE(SingleInstance::handOverToPreviousBuild(SingleInstance::launchFor(vaultA.path()), vaultA.path(), "tok"));
    ASSERT_TRUE(window.waitForNewConnection(1000));
    QLocalSocket *got = window.nextPendingConnection();
    ASSERT_TRUE(got->waitForReadyRead(1000));
    EXPECT_EQ(got->readLine(), QByteArray("tok\t" + vaultA.path().toUtf8() + "\n"));
    // A launch that names no vault goes to a window that is open: the settings that say which are not moved yet.
    EXPECT_TRUE(SingleInstance::handOverToPreviousBuild(SingleInstance::launchFor(QString()), QString(), "t2"));

    held.unlock();
    window.close();
    EXPECT_TRUE(SingleInstance::previousBuildSockets().isEmpty()) << "no owner, no window";
}

namespace {

/// A message handler that keeps what it is given, for a test that expects a warning.
QStringList heardMessages;
void hear(QtMsgType, const QMessageLogContext &, const QString &message)
{
    heardMessages.append(message);
}

} // namespace

TEST(SingleInstance, ALaunchWithNoPrivateDirectoryForItsSocketSaysSo)
{
    int argc = 1;
    char name[] = "test_xdg";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    Environment environment;
    QTemporaryDir home;
    qunsetenv("NOTEAPPC_INSTANCE_SOCKET");
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_STATE_HOME", home.filePath(QStringLiteral("state")).toUtf8());
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    const QString run = home.filePath(QStringLiteral("run"));
    ASSERT_TRUE(QDir().mkpath(run));
    qputenv("XDG_RUNTIME_DIR", run.toUtf8());
    // Both places exist already, and are open to others: a restored backup, a hand-made chmod.
    const auto open = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadOther
                      | QFileDevice::ExeOther;
    for (const QString &directory : {run + QStringLiteral("/notolith"), home.filePath(QStringLiteral("state/notolith"))}) {
        ASSERT_TRUE(QDir().mkpath(directory));
        ASSERT_TRUE(QFile::setPermissions(directory, open));
    }
    ASSERT_TRUE(SingleInstance::baseName().isEmpty());

    SingleInstance instance(SingleInstance::launchFor(QString()));
    heardMessages.clear();
    const QtMessageHandler before = qInstallMessageHandler(hear);
    instance.serve(nullptr);
    qInstallMessageHandler(before);

    ASSERT_EQ(heardMessages.filter(QStringLiteral("no private directory")).size(), 1);
}

TEST(SingleInstance, ALockItsOwnerLeftIsStaleEvenWhereItCannotBeCleared)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "a directory closed to the owner is open to root";
    Environment environment;
    QTemporaryDir dir;
    // The override is a prefix: `<dir>/win-<id>.sock` and its `.lock`.
    qputenv("NOTEAPPC_INSTANCE_SOCKET", (dir.path() + QStringLiteral("/win")).toUtf8());
    const QString dead = dir.path() + QStringLiteral("/win-dead.sock");
    const QString alive = dir.path() + QStringLiteral("/win-alive.sock");
    // A process that took the lock and ended without giving it back.
    const pid_t child = ::fork();
    if (child == 0) {
        QLockFile lock(dead + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        ::_exit(lock.tryLock() ? 0 : 1);
    }
    ASSERT_GT(child, 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    QLockFile held(alive + QStringLiteral(".lock"));
    held.setStaleLockTime(0);
    ASSERT_TRUE(held.tryLock());

    // The directory takes no writes (a full or read-only runtime directory): the stale lock cannot be cleared
    // away, and that is not a window.
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const QStringList open = SingleInstance::ownSockets();
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    EXPECT_TRUE(QFileInfo::exists(dead + QStringLiteral(".lock"))) << "it could not be cleared, as the test set up";
    EXPECT_EQ(open, (QStringList{alive})) << "the live lock is still a window";
}

TEST(StorageSettle, NothingMovesUnderAWindowOfThisBuild)
{
    int argc = 1;
    char name[] = "test_xdg";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    OldLayout machine;
    ASSERT_NO_FATAL_FAILURE(machine.populate());
    QTemporaryDir run, other;
    ASSERT_TRUE(QDir(machine.vaultPath).mkpath(QStringLiteral(".obsidian")));
    ASSERT_TRUE(QDir(other.path()).mkpath(QStringLiteral(".obsidian")));
    qunsetenv("NOTEAPPC_INSTANCE_SOCKET");
    qputenv("XDG_RUNTIME_DIR", run.path().toUtf8());

    // A window of this build on the vault, up before the old directories arrived (a restored backup).
    const QString socket = SingleInstance::nameForVault(machine.vaultPath);
    QLockFile held(socket + QStringLiteral(".lock"));
    held.setStaleLockTime(0);
    ASSERT_TRUE(held.tryLock());
    QLocalServer window;
    ASSERT_TRUE(window.listen(socket));
    const QString oldSettings = machine.config + QStringLiteral("/noteAppC/settings.ini");
    const auto untouched = [&] {
        EXPECT_TRUE(QFileInfo::exists(oldSettings));
        EXPECT_TRUE(QFileInfo::exists(machine.data + QStringLiteral("/noteAppC/recovery/n1/path")));
        EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/settings.ini")));
        EXPECT_FALSE(QFileInfo::exists(machine.data + QStringLiteral("/notolith/vaults")));
        EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Fresh);
    };

    // A launch for that vault goes to the window, and moves nothing.
    QStringList lines, warnings;
    EXPECT_EQ(settleStorage(machine.vaultPath, &lines, &warnings), StorageSettled::HandedOver);
    ASSERT_TRUE(window.waitForNewConnection(1000));
    QLocalSocket *got = window.nextPendingConnection();
    ASSERT_TRUE(got->waitForReadyRead(1000));
    EXPECT_TRUE(got->readLine().endsWith(machine.vaultPath.toUtf8() + "\n"));
    untouched();
    // A launch for another vault starts a window of its own, and moves nothing either: the first one is open.
    EXPECT_EQ(settleStorage(other.path(), &lines, &warnings), StorageSettled::Carry);
    untouched();
    EXPECT_TRUE(lines.isEmpty());
    // ...and says so, once, or a window that is always open would put it off without a word.
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings.first().contains(QStringLiteral("waiting"))) << warnings.first().toStdString();
    warnings.clear();

    // With no window open the next start does it.
    held.unlock();
    window.close();
    EXPECT_EQ(settleStorage(machine.vaultPath, &lines, &warnings), StorageSettled::Carry);
    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_FALSE(lines.isEmpty());
    EXPECT_TRUE(warnings.isEmpty()) << "nothing is waiting once it has moved";
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
}

TEST(MessageLog, TheFileIsPrivateFromTheMomentItExistsWhateverTheUmask)
{
    QTemporaryDir dir;
    const mode_t before = ::umask(0);
    const QString path = dir.filePath(QStringLiteral("notolith.log"));
    {
        MessageLog log(path, dir.filePath(QStringLiteral("notolith.log.lock")));
        ASSERT_TRUE(log.isOpen());
        EXPECT_EQ(mode(path), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    // One an older build made wider is narrowed.
    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
    {
        MessageLog log(path, dir.filePath(QStringLiteral("notolith.log.lock")));
        EXPECT_EQ(mode(path), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    ::umask(before);
}
