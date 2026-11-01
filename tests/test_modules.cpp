// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// The loadable-module tier (docs/INK-DESIGN.md
// section 4): discovery by metadata does not load the library; a module loads
// on first use; a wrong ABI tag, a duplicate suffix claim, a manifest without
// an id, a library that is not a module and an unresolvable library are each
// reported, never loaded or dropped silently.
#include <gtest/gtest.h>

#include <link.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtPlugin>
#include <QTemporaryDir>

#include "LoadedObjects.h"
#include "ModuleHost.h"
#include "ModuleRegistry.h"

using namespace noteAppC;

#ifdef NOTEAPPC_STATIC_FAKE
// The static fake module (tests/fakemodule/FakeStaticModule.cpp), linked into this executable: what an
// NOTEAPPC_STATIC_MODULES build does for every module in the app (apps/noteAppC/main.cpp).
Q_IMPORT_PLUGIN(FakeStaticModule)
#endif

namespace {

QString fakeDir() { return QStringLiteral(NOTEAPPC_FAKE_MODULES_DIR); }

const ModuleInfo *byID(const QList<ModuleInfo> &modules, const char *id)
{
    for (const ModuleInfo &module : modules)
        if (module.id == QLatin1String(id))
            return &module;
    return nullptr;
}

/// Sets an environment variable for one scope and puts the previous state back
/// after it: a mark or a module directory must not leak into the next test.
class EnvGuard {
public:
    EnvGuard(const char *name, const QByteArray &value)
        : m_name(name), m_hadPrevious(qEnvironmentVariableIsSet(name)), m_previous(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~EnvGuard()
    {
        if (m_hadPrevious)
            qputenv(m_name, m_previous);
        else
            qunsetenv(m_name);
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

private:
    const char *m_name;
    bool m_hadPrevious;
    QByteArray m_previous;
};

/// The modules a scan found in directories: a build with static modules lists those too, every scan.
QList<ModuleInfo> fromFiles(QList<ModuleInfo> modules)
{
    modules.removeIf([](const ModuleInfo &module) { return module.isStatic; });
    return modules;
}

/// A registry over `modules` with every feature on (a QObject: it cannot be returned by value).
void useModules(ModuleRegistry &registry, const QList<ModuleInfo> &modules)
{
    registry.setModules(modules);
    registry.setEnabledCheck([](const QString &) { return true; });
}

} // namespace

// Its own ctest entry (`modules_gate`, tests/CMakeLists.txt): no other test in the
// process may have mapped fake_good first, or "the scan did not load it" proves nothing.
TEST(ModuleGateTest, ScanReadsManifestsWithoutLoadingTheLibrary)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    EnvGuard markGuard("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *good = byID(modules, "fake");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->suffixes, QStringList{QStringLiteral("fake")});
    ASSERT_EQ(good->commands.size(), 3);
    EXPECT_EQ(good->commands.first().id, QStringLiteral("fake:hello"));
    EXPECT_EQ(good->ABItag, ModuleRegistry::ABItag());
    EXPECT_TRUE(good->problem.isEmpty()) << good->problem.toStdString();
    EXPECT_FALSE(QFile::exists(mark)) << "the static initializer ran: metaData() loaded the library";
    EXPECT_FALSE(loadedObjects().contains(QFileInfo(good->path).canonicalFilePath()));
}

TEST(ModuleRegistryTest, StrayFilesInTheDirectoryAreSkippedSilently)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(QFile(tmp.filePath(QStringLiteral("README"))).open(QIODevice::WriteOnly));
    ASSERT_TRUE(QFile(tmp.filePath(QStringLiteral("libfoo.so.1"))).open(QIODevice::WriteOnly));
    QFile notAPlugin(tmp.filePath(QStringLiteral("junk.so")));
    ASSERT_TRUE(notAPlugin.open(QIODevice::WriteOnly));
    notAPlugin.write("not an ELF file");
    notAPlugin.close();
    EXPECT_TRUE(fromFiles(ModuleRegistry::scan({tmp.path()})).isEmpty());
    EXPECT_TRUE(fromFiles(ModuleRegistry::scan({tmp.filePath(QStringLiteral("missing"))})).isEmpty());
}

TEST(ModuleRegistryTest, LoadsOnFirstUseAndAttachesOnce)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    EnvGuard markGuard("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    ModuleRegistry registry;
    useModules(registry, ModuleRegistry::scan({fakeDir()}));
    const std::optional<ModuleInfo> good = registry.moduleForSuffix(QStringLiteral("/v/a.fake"));
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ(good->id, QStringLiteral("fake"));
    EXPECT_FALSE(registry.isLoaded(good->id));

    int views = 0, commands = 0, settings = 0;  // the fake registers 3 commands and 2 setting rows
    ModuleHost host;
    host.registerView = [&](const QStringList &suffixes, ModuleHost::ViewFactory) { views += suffixes.size(); };
    host.registerCommand = [&](const QString &, const QString &, std::function<void()>) { ++commands; };
    host.registerSetting = [&](const ModuleHost::SettingRow &) { ++settings; };
    QString error;
    ASSERT_TRUE(registry.ensureLoaded(*good, host, &error)) << error.toStdString();
    EXPECT_TRUE(registry.isLoaded(good->id));
    EXPECT_TRUE(QFile::exists(mark));
    EXPECT_TRUE(loadedObjects().contains(QFileInfo(good->path).canonicalFilePath()));
    EXPECT_EQ(views, 1);
    EXPECT_EQ(commands, 3);
    EXPECT_EQ(settings, 2);
    ASSERT_TRUE(registry.ensureLoaded(*good, host, &error)); // a second call attaches nothing again
    EXPECT_EQ(views, 1);
}

TEST(ModuleRegistryTest, WrongABItagIsRefusedWithANoticeNamingTheModule)
{
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *bad = byID(modules, "fake-badabi");
    ASSERT_NE(bad, nullptr);
    EXPECT_FALSE(bad->problem.isEmpty());
    EXPECT_TRUE(bad->problem.contains(QStringLiteral("fake-badabi")));
    EXPECT_TRUE(bad->problem.contains(ModuleRegistry::ABItag()));
    ModuleRegistry registry;
    useModules(registry, modules);
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*bad, host, &error));
    EXPECT_EQ(error, bad->problem);
    EXPECT_FALSE(registry.isLoaded(bad->id));
}

// M3: fake_badabi sorts before fake_good and claims the same suffix, but it has a
// problem, so it never owns the suffix; fake_good is the first problem-free claimant
// and fake_twin, which comes later, is refused.
TEST(ModuleRegistryTest, ASecondModuleClaimingAClaimedSuffixIsRefusedAndTheFirstKeepsIt)
{
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *first = byID(modules, "fake");
    const ModuleInfo *twin = byID(modules, "fake-twin");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(twin, nullptr);
    EXPECT_TRUE(first->problem.isEmpty()) << first->problem.toStdString();
    EXPECT_TRUE(twin->problem.contains(QStringLiteral("claims .fake"))) << twin->problem.toStdString();
    EXPECT_TRUE(twin->problem.contains(QStringLiteral("already claims")));
    EXPECT_TRUE(twin->problem.contains(QStringLiteral("fake-twin")));
    ModuleRegistry registry;
    useModules(registry, modules);
    const std::optional<ModuleInfo> owner = registry.moduleForSuffix(QStringLiteral("a.fake"));
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(owner->id, QStringLiteral("fake"));
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*twin, host, &error));
    EXPECT_EQ(error, twin->problem);
    EXPECT_FALSE(registry.isLoaded(twin->id));
}

// M6: a manifest with an empty id is listed, named by its file, not dropped silently.
TEST(ModuleRegistryTest, AManifestWithoutAnIDIsReportedAndNeverLoaded)
{
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *noID = byID(modules, "fake_noid"); // the id shown is the file's base name
    ASSERT_NE(noID, nullptr);
    EXPECT_TRUE(noID->problem.contains(QStringLiteral("manifest has no id"))) << noID->problem.toStdString();
    EXPECT_TRUE(noID->problem.contains(QStringLiteral("fake_noid.so")));
    ModuleRegistry registry;
    useModules(registry, modules);
    EXPECT_FALSE(registry.moduleForSuffix(QStringLiteral("a.fakenoid")).has_value());
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*noID, host, &error));
    EXPECT_EQ(error, noID->problem);
}

// M6: a Qt plugin with the right IID that does not implement NotolithModule.
TEST(ModuleRegistryTest, ALibraryThatIsNotAModuleIsReportedAndStaysUnattached)
{
    ModuleRegistry registry;
    useModules(registry, ModuleRegistry::scan({fakeDir()}));
    const std::optional<ModuleInfo> module = registry.moduleForSuffix(QStringLiteral("a.fakenotmod"));
    ASSERT_TRUE(module.has_value());
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*module, host, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("not a Notolith module"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("fake-notmodule")));
    EXPECT_FALSE(registry.isLoaded(module->id));
    EXPECT_EQ(registry.loadFailure(module->id), error);
}

// No dependency check. A module whose library cannot resolve a symbol (an absent
// inkcore, in life) fails with the loader's message, which Settings shows.
TEST(ModuleRegistryTest, AnUnresolvedSymbolFailsTheLoadWithTheLoadersMessage)
{
    ModuleRegistry registry;
    useModules(registry, ModuleRegistry::scan({fakeDir()}));
    const std::optional<ModuleInfo> module = registry.moduleForCommand(QStringLiteral("fake-unresolved:hello"));
    ASSERT_TRUE(module.has_value());
    EXPECT_TRUE(module->problem.isEmpty()) << "nothing is wrong until the library is opened";
    EXPECT_TRUE(registry.loadFailure(module->id).isEmpty());
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*module, host, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("noteappcFakeMissingSymbol"))) << error.toStdString();
    EXPECT_EQ(registry.loadFailure(module->id), error);
    EXPECT_FALSE(registry.isLoaded(module->id));
    QString again;
    EXPECT_FALSE(registry.ensureLoaded(*module, host, &again));
    EXPECT_EQ(again, error) << "a failed library is not retried";
}

TEST(ModuleRegistryTest, AModuleMayNotClaimASuffixTheAppOpensItself)
{
    const QList<ModuleInfo> modules =
        ModuleRegistry::scan({fakeDir()}, [](const QString &suffix) { return suffix == QLatin1String("fake"); });
    const ModuleInfo *claimant = byID(modules, "fake");
    ASSERT_NE(claimant, nullptr);
    EXPECT_TRUE(claimant->problem.contains(QStringLiteral("built-in file type .fake"))) << claimant->problem.toStdString();
    EXPECT_TRUE(claimant->problem.contains(QStringLiteral("fake")));
    ModuleRegistry registry;
    useModules(registry, modules);
    EXPECT_FALSE(registry.moduleForSuffix(QStringLiteral("a.fake")).has_value()) << "the built-in kind keeps the suffix";
    const ModuleInfo *other = byID(modules, "fake-notmodule");
    ASSERT_NE(other, nullptr);
    EXPECT_TRUE(other->problem.isEmpty()) << "a module with other suffixes is not affected";
}

TEST(ModuleRegistryTest, DisabledFeatureUnregistersTheSuffixAndCommand)
{
    ModuleRegistry registry;
    registry.setModules(ModuleRegistry::scan({fakeDir()}));
    bool on = true;
    registry.setEnabledCheck([&on](const QString &featureID) { return featureID != QLatin1String("fake") || on; });
    EXPECT_TRUE(registry.moduleForSuffix(QStringLiteral("a.FAKE")).has_value());
    EXPECT_TRUE(registry.moduleForCommand(QStringLiteral("fake:hello")).has_value());
    on = false;
    EXPECT_FALSE(registry.moduleForSuffix(QStringLiteral("a.fake")).has_value());
    EXPECT_FALSE(registry.moduleForCommand(QStringLiteral("fake:hello")).has_value());
}

// The tag is version, a random token made once per build directory, and the Qt version.
TEST(ModuleRegistryTest, ABItagIsVersionTokenAndQtVersion)
{
    const QStringList parts = ModuleRegistry::ABItag().split(QLatin1Char('-'));
    ASSERT_EQ(parts.size(), 3) << ModuleRegistry::ABItag().toStdString();
    EXPECT_FALSE(parts[0].isEmpty());
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[0-9a-f]{16}$")).match(parts[1]).hasMatch()) << parts[1].toStdString();
    EXPECT_EQ(parts[2], QStringLiteral(QT_VERSION_STR));
}

#ifdef NOTEAPPC_INK_MODULE_PATH
// No dependency check in the manifest. An ink module copied to where libnotolith-inkcore.so is
// not (RUNPATH $ORIGIN is its own directory) fails to load with the loader's message, which Settings
// shows, instead of crashing. This process never loads the core library, so nothing satisfies the
// NEEDED entry by soname.
TEST(ModuleRegistryTest, AnInkModuleWithoutItsCoreLibraryFailsToLoadWithTheLoadersMessage)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(QFile::copy(QStringLiteral(NOTEAPPC_INK_MODULE_PATH), tmp.filePath(QStringLiteral("notolith-ink.so"))));
    const QList<ModuleInfo> modules = ModuleRegistry::scan({tmp.path()});
    ASSERT_EQ(modules.size(), 1);
    ASSERT_TRUE(modules.first().problem.isEmpty()) << modules.first().problem.toStdString();
    EXPECT_EQ(modules.first().id, QStringLiteral("ink"));
    ModuleRegistry registry;
    useModules(registry, modules);
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(modules.first(), host, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("libnotolith-inkcore.so"))) << error.toStdString();
    EXPECT_FALSE(registry.isLoaded(QStringLiteral("ink")));
    EXPECT_EQ(registry.loadFailure(QStringLiteral("ink")), error) << "Settings shows what the registry kept";
}
#endif

// M10: no per-user module directory.
TEST(ModuleRegistryTest, DefaultDirectoriesAreTheOverrideOrTheLibdirAlone)
{
    {
        EnvGuard guard("NOTEAPPC_MODULE_DIRS", "/tmp/a:/tmp/b");
        EXPECT_EQ(ModuleRegistry::defaultDirectories(), (QStringList{QStringLiteral("/tmp/a"), QStringLiteral("/tmp/b")}));
    }
    EnvGuard unset("NOTEAPPC_MODULE_DIRS", QByteArray());
    const QStringList dirs = ModuleRegistry::defaultDirectories();
    ASSERT_EQ(dirs.size(), 1);
    EXPECT_TRUE(dirs.first().endsWith(QStringLiteral("/notolith/modules")));
    EXPECT_FALSE(dirs.first().startsWith(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)))
        << "$XDG_DATA_HOME is not searched";
}

// M8: ctest hands every test an empty module directory unless it sets its own, so
// what is installed on the machine never reaches a test.
TEST(ModuleRegistryTest, TheTestEnvironmentPointsTheRegistryAtAnEmptyDirectory)
{
    ASSERT_TRUE(qEnvironmentVariableIsSet("NOTEAPPC_MODULE_DIRS")) << "run through ctest";
    EXPECT_TRUE(fromFiles(ModuleRegistry::scan(ModuleRegistry::defaultDirectories())).isEmpty());
}

// A module found in a directory has a path and is not static, in either configuration.
TEST(ModuleRegistryTest, AModuleFoundInADirectoryIsNotStaticAndHasAPath)
{
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *good = byID(modules, "fake");
    ASSERT_NE(good, nullptr);
    EXPECT_FALSE(good->isStatic);
    EXPECT_FALSE(good->path.isEmpty());
}

#ifdef NOTEAPPC_STATIC_FAKE
// A module linked into the executable (Qt static plugin, Android has no directory to scan) is
// listed from QPluginLoader::staticPlugins() by the same IID check, with no path, and loaded only when used.
TEST(ModuleStaticTest, AStaticModuleIsListedWithoutADirectoryAndWithoutBeingInstantiated)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    EnvGuard markGuard("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    const QList<ModuleInfo> modules = ModuleRegistry::scan({});
    const ModuleInfo *fake = byID(modules, "fake-static");
    ASSERT_NE(fake, nullptr);
    EXPECT_TRUE(fake->isStatic);
    EXPECT_TRUE(fake->path.isEmpty());
    EXPECT_EQ(fake->suffixes, QStringList{QStringLiteral("fakestatic")});
    ASSERT_EQ(fake->commands.size(), 1);
    EXPECT_EQ(fake->commands.first().id, QStringLiteral("fake-static:hello"));
    EXPECT_EQ(fake->ABItag, ModuleRegistry::ABItag());
    EXPECT_TRUE(fake->problem.isEmpty()) << fake->problem.toStdString();
    EXPECT_FALSE(QFile::exists(mark)) << "the scan constructed the module: metaData() must not";
}

TEST(ModuleStaticTest, AStaticModuleLoadsOnFirstUseAndAttachesOnce)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    EnvGuard markGuard("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    ModuleRegistry registry;
    useModules(registry, ModuleRegistry::scan({}));
    const std::optional<ModuleInfo> fake = registry.moduleForSuffix(QStringLiteral("/v/a.fakestatic"));
    ASSERT_TRUE(fake.has_value());
    EXPECT_EQ(fake->id, QStringLiteral("fake-static"));
    ASSERT_TRUE(registry.moduleForCommand(QStringLiteral("fake-static:hello")).has_value());
    EXPECT_FALSE(registry.isLoaded(fake->id));
    EXPECT_FALSE(QFile::exists(mark));

    int commands = 0;
    QStringList viewSuffixes;
    ModuleHost host;
    host.registerView = [&](const QStringList &suffixes, ModuleHost::ViewFactory) { viewSuffixes += suffixes; };
    host.registerCommand = [&](const QString &, const QString &, std::function<void()>) { ++commands; };
    QString error;
    ASSERT_TRUE(registry.ensureLoaded(*fake, host, &error)) << error.toStdString();
    EXPECT_TRUE(registry.isLoaded(fake->id));
    EXPECT_EQ(commands, 1);
    EXPECT_EQ(viewSuffixes, QStringList{QStringLiteral("fakestatic")});
    EXPECT_TRUE(QFile::exists(mark)) << "the module is constructed on the first load";
    ASSERT_TRUE(registry.ensureLoaded(*fake, host, &error));
    EXPECT_EQ(commands, 1) << "a second ensureLoaded does not attach again";
}

TEST(ModuleStaticTest, AStaticEntryWithNoModuleBehindItFailsToLoadAndSaysSo)
{
    ModuleInfo ghost;
    ghost.id = QStringLiteral("ghost");
    ghost.featureID = ghost.id;
    ghost.isStatic = true;
    ModuleRegistry registry;
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(ghost, host, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(registry.loadFailure(QStringLiteral("ghost")), error);
    EXPECT_FALSE(registry.isLoaded(QStringLiteral("ghost")));
}
#endif

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); // views are widgets
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
