// The loadable-module tier (docs/superpowers/specs/2026-10-21-ink-core-design.md
// section 4): discovery by metadata does not load the library; a module loads
// on first use; a wrong ABI tag or a missing dependency is reported, not loaded.
#include <gtest/gtest.h>

#include <link.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "ModuleHost.h"
#include "ModuleRegistry.h"

using namespace noteAppC;

namespace {

/// Every shared object mapped into this process, by absolute path.
QStringList loadedObjects()
{
    QStringList out;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) {
            if (info->dlpi_name && *info->dlpi_name)
                static_cast<QStringList *>(data)->append(QFileInfo(QString::fromLocal8Bit(info->dlpi_name)).canonicalFilePath());
            return 0;
        },
        &out);
    return out;
}

QString fakeDir() { return QStringLiteral(NOTEAPPC_FAKE_MODULES_DIR); }

const ModuleInfo *byID(const QList<ModuleInfo> &modules, const char *id)
{
    for (const ModuleInfo &module : modules)
        if (module.id == QLatin1String(id))
            return &module;
    return nullptr;
}

} // namespace

TEST(ModuleRegistryTest, ScanReadsManifestsWithoutLoadingTheLibrary)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    qputenv("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *good = byID(modules, "fake");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->suffixes, QStringList{QStringLiteral("fake")});
    EXPECT_TRUE(good->indexSuffixes.isEmpty());
    ASSERT_EQ(good->commands.size(), 2);
    EXPECT_EQ(good->commands.first().id, QStringLiteral("fake:hello"));
    const ModuleInfo *indexed = byID(modules, "fake-indexed");
    ASSERT_NE(indexed, nullptr);
    EXPECT_EQ(indexed->indexSuffixes, QStringList{QStringLiteral("fakeidx")});
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
    EXPECT_TRUE(ModuleRegistry::scan({tmp.path()}).isEmpty());
    EXPECT_TRUE(ModuleRegistry::scan({tmp.filePath(QStringLiteral("missing"))}).isEmpty());
}

TEST(ModuleRegistryTest, LoadsOnFirstUseAndAttachesOnce)
{
    QTemporaryDir tmp;
    const QString mark = tmp.filePath(QStringLiteral("mark"));
    qputenv("NOTEAPPC_FAKE_MODULE_MARK", mark.toLocal8Bit());
    ModuleRegistry registry;
    registry.setModules(ModuleRegistry::scan({fakeDir()}));
    registry.setEnabledCheck([](const QString &) { return true; });
    const ModuleInfo *good = registry.moduleForSuffix(QStringLiteral("/v/a.fake"));
    ASSERT_NE(good, nullptr);
    EXPECT_FALSE(registry.isLoaded(good->id));

    int views = 0, commands = 0, settings = 0;  // the fake registers 2 commands and 2 setting rows
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
    EXPECT_EQ(commands, 2);
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
    registry.setModules(modules);
    registry.setEnabledCheck([](const QString &) { return true; });
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*bad, host, &error));
    EXPECT_EQ(error, bad->problem);
    EXPECT_FALSE(registry.isLoaded(bad->id));
}

TEST(ModuleRegistryTest, MissingDependencyIsReportedAndNotLoaded)
{
    const QList<ModuleInfo> modules = ModuleRegistry::scan({fakeDir()});
    const ModuleInfo *dep = byID(modules, "fake-missingdep");
    ASSERT_NE(dep, nullptr);
    EXPECT_TRUE(dep->problem.contains(QStringLiteral("nonexistent-module")));
    ModuleRegistry registry;
    registry.setModules(modules);
    registry.setEnabledCheck([](const QString &) { return true; });
    EXPECT_EQ(registry.moduleForSuffix(QStringLiteral("x.fake"))->id, QStringLiteral("fake")); // the good one wins
    ModuleHost host;
    QString error;
    EXPECT_FALSE(registry.ensureLoaded(*dep, host, &error));
    EXPECT_EQ(error, dep->problem);
    EXPECT_FALSE(registry.isLoaded(dep->id));
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
    registry.setModules(modules);
    registry.setEnabledCheck([](const QString &) { return true; });
    EXPECT_EQ(registry.moduleForSuffix(QStringLiteral("a.fake")), nullptr) << "the built-in kind keeps the suffix";
    const ModuleInfo *other = byID(modules, "fake-indexed");
    ASSERT_NE(other, nullptr);
    EXPECT_TRUE(other->problem.isEmpty()) << "a module with other suffixes is not affected";
}

TEST(ModuleRegistryTest, DisabledFeatureUnregistersTheSuffixAndCommand)
{
    ModuleRegistry registry;
    registry.setModules(ModuleRegistry::scan({fakeDir()}));
    bool on = true;
    registry.setEnabledCheck([&on](const QString &featureID) { return featureID != QLatin1String("fake") || on; });
    EXPECT_NE(registry.moduleForSuffix(QStringLiteral("a.FAKE")), nullptr);
    EXPECT_NE(registry.moduleForCommand(QStringLiteral("fake:hello")), nullptr);
    on = false;
    EXPECT_EQ(registry.moduleForSuffix(QStringLiteral("a.fake")), nullptr);
    EXPECT_EQ(registry.moduleForCommand(QStringLiteral("fake:hello")), nullptr);
}

TEST(ModuleRegistryTest, DefaultDirectoriesHonourTheOverride)
{
    qputenv("NOTEAPPC_MODULE_DIRS", "/tmp/a:/tmp/b");
    EXPECT_EQ(ModuleRegistry::defaultDirectories(), (QStringList{QStringLiteral("/tmp/a"), QStringLiteral("/tmp/b")}));
    qunsetenv("NOTEAPPC_MODULE_DIRS");
    const QStringList dirs = ModuleRegistry::defaultDirectories();
    ASSERT_EQ(dirs.size(), 2);
    EXPECT_TRUE(dirs.first().endsWith(QStringLiteral("/noteAppC/modules")));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); // views are widgets
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
