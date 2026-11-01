// Where Notolith keeps its files: the XDG helper, the migration from the old noteAppC directories, and the
// log file two vault processes share.

#include <gtest/gtest.h>

#include <QCoreApplication>
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
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Retry) << "a later start tries again";
    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/settings.ini")), QByteArray("[editor]\ntab-width = 8\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/notolith/recovery/n1/path")), QByteArray("/mine/Note.md"));
    // What could not move stays where it was, for the person to look at.
    EXPECT_EQ(read(machine.config + QStringLiteral("/noteAppC/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/noteAppC/recovery/n1/path")), QByteArray("/x/Note.md"));
    // Everything else did move.
    EXPECT_TRUE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/themes/Mine.json")));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC/themes")));

    // The person sorts the clash out; the next start finishes the job and ends it.
    QFile::remove(machine.config + QStringLiteral("/notolith/settings.ini"));
    QFile::remove(machine.data + QStringLiteral("/notolith/recovery/n1/path"));
    warnings.clear();
    EXPECT_EQ(migrateStorage(nullptr, &warnings), 2);
    EXPECT_TRUE(warnings.isEmpty());
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::None);
    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC")));
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
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Retry);
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
    EXPECT_EQ(storageMigrationNeed(), MigrationNeed::Retry);
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
