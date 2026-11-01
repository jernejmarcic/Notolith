// Where Notolith keeps its files: the XDG helper, the migration from the old noteAppC directories, and the
// log file two vault processes share.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include "ConfigFile.h"
#include "MessageLog.h"
#include "Settings.h"
#include "StorageMigration.h"

#include <QCryptographicHash>

using namespace noteAppC;

namespace {

/// Every variable the helper reads, put back after the test.
struct Environment {
    static constexpr const char *names[] = {"HOME",           "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                                            "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_DATA_DIRS", "XDG_CONFIG_DIRS"};
    QByteArray saved[8];
    bool wasSet[8];
    Environment()
    {
        for (int i = 0; i < 8; ++i) {
            wasSet[i] = qEnvironmentVariableIsSet(names[i]);
            saved[i] = qgetenv(names[i]);
        }
    }
    ~Environment()
    {
        for (int i = 0; i < 8; ++i) {
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
    EXPECT_FALSE(QFileInfo::exists(s + QStringLiteral("vaults/") + machine.strangerKey)) << "a vault nobody remembers";
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
    EXPECT_FALSE(window.contains(QStringLiteral("publish/") + machine.strangerKey + QStringLiteral("/folder")));
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

    migrateStorage();

    EXPECT_EQ(read(machine.config + QStringLiteral("/notolith/settings.ini")), QByteArray("[editor]\ntab-width = 8\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/notolith/recovery/n1/path")), QByteArray("/mine/Note.md"));
    // What could not move stays where it was, for the person to look at.
    EXPECT_EQ(read(machine.config + QStringLiteral("/noteAppC/settings.ini")), QByteArray("[editor]\ntab-width = 2\n"));
    EXPECT_EQ(read(machine.data + QStringLiteral("/noteAppC/recovery/n1/path")), QByteArray("/x/Note.md"));
    // Everything else did move.
    EXPECT_TRUE(QFileInfo::exists(machine.config + QStringLiteral("/notolith/themes/Mine.json")));
    EXPECT_FALSE(QFileInfo::exists(machine.config + QStringLiteral("/noteAppC/themes")));
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
