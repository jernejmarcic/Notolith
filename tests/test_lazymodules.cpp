// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// What a start maps, and what a module costs until it is wanted: poppler and the share stack live in
// loadable modules (libs/ext/pdfview, libs/ext/shareengine), so a session that never meets a PDF, and one that never
// shares, maps none of their libraries. Each test is its own ctest entry (tests/CMakeLists.txt): what is mapped is a
// fact about the whole process, and an earlier test in it would already have mapped what a later one asserts absent.
#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QListWidget>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfWriter>
#include <QPainter>
#include <QStatusBar>
#include <QTemporaryDir>

#include "LoadedObjects.h"
#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "ModuleHost.h"
#include "ModuleRegistry.h"
#include "PdfPage.h"
#include "Settings.h"
#include "Workspaces.h"
#ifdef NOTEAPPC_HAVE_SHARE
#include "SecretStore.h"
#include "ShareEngineBase.h"
#endif
#include "WaitFor.h"

using namespace noteAppC;

namespace {

const QStringList kPoppler = {QStringLiteral("libpoppler")};
#ifdef NOTEAPPC_HAVE_SHARE
/// What the share engine brings: none of it may be mapped until sharing is wanted.
const QStringList kShareStack = {QStringLiteral("libprotobuf"), QStringLiteral("libdatachannel"),
                                 QStringLiteral("libavahi"), QStringLiteral("libabsl")};
#endif

/// A vault with one note that embeds nothing and a two-page PDF next to it.
class LazyVault {
public:
    LazyVault()
    {
        QFile note(m_dir.filePath(QStringLiteral("Note.md")));
        EXPECT_TRUE(note.open(QIODevice::WriteOnly));
        note.write("# Note\n\nNothing embedded here.\n");
        QPdfWriter writer(pdfPath());
        QPainter painter(&writer);
        painter.drawText(100, 100, QStringLiteral("first"));
        writer.newPage();
        painter.drawText(100, 100, QStringLiteral("second"));
    }
    QString root() const { return m_dir.path(); }
    QString notePath() const { return m_dir.filePath(QStringLiteral("Note.md")); }
    QString pdfPath() const { return m_dir.filePath(QStringLiteral("Paper.pdf")); }

private:
    QTemporaryDir m_dir;
};

bool anyMapped(const QStringList &needles)
{
    for (const QString &needle : needles)
        if (!loadedObjectsMatching(needle).isEmpty())
            return true;
    return false;
}

QString mappedNames(const QStringList &needles)
{
    QStringList names;
    for (const QString &needle : needles)
        names << loadedObjectsMatching(needle);
    return names.join(QLatin1Char(' '));
}

/// The Settings toggle of `feature`: the page builds a page when first shown, so look on each in turn, as a person clicks the nav.
QAbstractButton *featureToggle(MainWindow &window, const char *feature)
{
    window.openSettings();
    const QString name = QStringLiteral("settingsToggle_") + Settings::featureKey(feature);
    if (auto *found = window.findChild<QAbstractButton *>(name))
        return found;
    auto *nav = window.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    for (int row = 0; nav && row < nav->count(); ++row) {
        nav->setCurrentRow(row);
        if (auto *found = window.findChild<QAbstractButton *>(name))
            return found;
    }
    return nullptr;
}

/// Starts the way main() does (openTarget runs after the first paint) and waits for the module scan that follows it.
void startAndScan(MainWindow &window, const QString &vaultRoot)
{
    window.show();
    window.openTarget(resolveLaunchTarget(vaultRoot));
    QApplication::processEvents();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window.modules()->modules().isEmpty(); }, 10000)) << "no module scan result";
}

/// The workspace a session left behind: one split, one group, one tab showing `file` (a PDF, which only a module opens).
void seedWorkspaceWith(const QString &vaultRoot, const QString &file)
{
    const QJsonObject tab{{QStringLiteral("id"), QStringLiteral("t1")}, {QStringLiteral("type"), QStringLiteral("leaf")},
                          {QStringLiteral("state"), QJsonObject{{QStringLiteral("type"), QStringLiteral("pdf")},
                                                               {QStringLiteral("state"), QJsonObject{{QStringLiteral("file"), file}}}}}};
    const QJsonObject group{{QStringLiteral("id"), QStringLiteral("g1")}, {QStringLiteral("type"), QStringLiteral("tabs")},
                            {QStringLiteral("currentTab"), 0}, {QStringLiteral("children"), QJsonArray{tab}}};
    const QJsonObject main{{QStringLiteral("id"), QStringLiteral("s1")}, {QStringLiteral("type"), QStringLiteral("split")},
                           {QStringLiteral("direction"), QStringLiteral("vertical")}, {QStringLiteral("children"), QJsonArray{group}}};
    ASSERT_TRUE(Workspaces::saveCurrent(vaultRoot, {{QStringLiteral("main"), main}, {QStringLiteral("active"), QStringLiteral("t1")}}));
}

bool workspaceNames(const QString &vaultRoot, const QString &file)
{
    return QJsonDocument(Workspaces::current(vaultRoot)).toJson().contains(file.toUtf8());
}

} // namespace

// 1. A start with a note open, the scan done, maps no poppler; opening the PDF maps it.
TEST(LazyModules, PdfNothingMappedUntilAPdfIsOpened)
{
    LazyVault vault;
    MainWindow window;
    startAndScan(window, vault.root());
    ASSERT_NE(window.modules()->moduleForSuffix(vault.pdfPath()), std::nullopt) << "the pdfview module was not found";
    EXPECT_FALSE(anyMapped(kPoppler)) << mappedNames(kPoppler).toStdString();
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("pdfview")));
    EXPECT_TRUE(loadedObjectsMatching(QStringLiteral("libnotolith-pdfpage")).isEmpty());

    ASSERT_TRUE(window.openNote(vault.pdfPath()));
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("pdfview")));
    EXPECT_FALSE(loadedObjectsMatching(QStringLiteral("libpoppler-qt6")).isEmpty());
}

// 2. An embed maps poppler through the C face alone: no module attached, no tab code; a later tab reuses the mapping.
TEST(LazyModules, PdfEmbedResolvesWithoutThePluginFace)
{
    LazyVault vault;
    EXPECT_FALSE(anyMapped(kPoppler));
    EXPECT_TRUE(PdfPage::isAvailable());
    EXPECT_EQ(PdfPage::pageCount(vault.pdfPath()), 2);
    EXPECT_FALSE(PdfPage::render(vault.pdfPath(), 1, 300).isNull());
    EXPECT_EQ(PdfPage::pageCount(QStringLiteral("/nonexistent/none.pdf")), 0);
    EXPECT_FALSE(loadedObjectsMatching(QStringLiteral("libpoppler-qt6")).isEmpty());
    EXPECT_FALSE(loadedObjectsMatching(QStringLiteral("libnotolith-pdfpage")).isEmpty());
    EXPECT_TRUE(loadedObjectsMatching(QStringLiteral("notolith-pdfview.so")).isEmpty()) << "the plugin face stays unloaded";

    const QStringList before = loadedObjectsMatching(QStringLiteral("libpoppler-qt6"));
    MainWindow window;
    startAndScan(window, vault.root());
    ASSERT_TRUE(window.openNote(vault.pdfPath()));
    EXPECT_EQ(loadedObjectsMatching(QStringLiteral("libpoppler-qt6")), before) << "one mapping of poppler, not two";
}

// 6. The first open of a session is a PDF (a path on the command line, the last session's tab) and no scan has run.
TEST(LazyModules, PdfOpensBeforeAnyScan)
{
    LazyVault vault;
    MainWindow window;
    window.show();
    window.openTarget(resolveLaunchTarget(vault.pdfPath()));
    QApplication::processEvents();
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("pdfview")));
    EXPECT_EQ(window.findChildren<ModuleView *>().size(), 1) << "a tab, not the system viewer's refusal";
}

// 4. No module installed (the ctest entry points the registry at a directory that is not there).
TEST(LazyModules, PdfWithoutTheModuleIsNotAViewFile)
{
    LazyVault vault;
    MainWindow window;
    window.show();
    window.openTarget(resolveLaunchTarget(vault.root()));
    window.scanModulesNow(); // finds nothing, so there is no result to wait for
    EXPECT_FALSE(PdfPage::isAvailable());
    EXPECT_EQ(PdfPage::pageCount(vault.pdfPath()), 0);
    EXPECT_TRUE(PdfPage::render(vault.pdfPath(), 0, 300).isNull());
    EXPECT_FALSE(window.openNote(vault.pdfPath()));
    // The settings row is still there, switched off and naming the package.
    auto *toggle = featureToggle(window, "pdf-embeds");
    ASSERT_NE(toggle, nullptr) << "the PDF embeds row vanished";
    EXPECT_FALSE(toggle->isEnabled());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("notolith-pdfview")))
        << window.statusBar()->currentMessage().toStdString();
    // An embed of a perfectly good PDF says what is missing, not that the file is broken.
    MarkdownEditor editor;
    editor.resize(900, 600);
    editor.setNoteText(QStringLiteral("![[Paper.pdf]]\n"));
    editor.setLinkResolver([&](const QString &) { return vault.pdfPath(); });
    const MarkdownEditor::PdfEmbedInfo info = editor.pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_TRUE(info.error.contains(QStringLiteral("notolith-pdfview"))) << info.error.toStdString();
    EXPECT_FALSE(anyMapped(kPoppler));
}

#ifdef NOTEAPPC_HAVE_SHARE
void setShareFeature(bool on)
{
    Settings settings = Settings::load();
    settings.setValue(Settings::featureKey("share"), on);
    ASSERT_TRUE(settings.save());
}

/// A window on `vault` with the share engine pointed at throw-away places: nothing on the network, no real store.
void configureShare(MainWindow &window, const QString &storeDirectory)
{
    ShareEngineBase::Config config;
    config.port = 0;
    config.mDNS = false;
    config.identityKey = QStringLiteral("lazy-test-identity");
    config.hosts = {QStringLiteral("127.0.0.1")};
    config.storeDirectory = storeDirectory;
    window.setShareConfigForTests(config);
}

// 7. Sharing is on (its default) and the vault holds no share: the module stays on disk and nothing of its stack is mapped.
TEST(LazyModules, ShareNothingMappedWhileTheVaultHoldsNoShare)
{
    LazyVault vault;
    QTemporaryDir store;
    MainWindow window;
    configureShare(window, store.path());
    startAndScan(window, vault.root());
    QTest::qWait(300); // what updateShare would still do after the scan
    EXPECT_EQ(window.shareEngine(), nullptr);
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("share")));
    EXPECT_FALSE(anyMapped(kShareStack)) << mappedNames(kShareStack).toStdString();
}

// 8. A vault that holds a share resumes it: the module loads after the paint, with the scan, and the engine starts.
TEST(LazyModules, ShareLoadsWhenTheVaultHoldsAShare)
{
    LazyVault vault;
    QTemporaryDir store;
    ShareRecord record;
    record.shareID = QByteArray(16, '\x11');
    record.owner = QByteArray(32, '\x22');
    record.key = QByteArray(32, '\x33');
    record.members = QByteArray(8, '\x44'); // the store only needs it non-empty; the engine checks its signature when it starts
    record.backend = 2;
    record.clientID = 7;
    record.relativePath = QStringLiteral("Note.md");
    record.title = QStringLiteral("Note");
    record.owned = true;
    ASSERT_TRUE(ShareStore(store.path()).save(record, QByteArray(), QStringLiteral("# Note\n")));
    MainWindow window;
    configureShare(window, store.path());
    window.show();
    window.openTarget(resolveLaunchTarget(vault.root()));
    EXPECT_FALSE(anyMapped(kShareStack)) << "not before the scan: " << mappedNames(kShareStack).toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return window.shareEngine() != nullptr; }, 10000)) << "the engine was not built";
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("share")));
    EXPECT_FALSE(loadedObjectsMatching(QStringLiteral("libprotobuf")).isEmpty());
    EXPECT_TRUE(QTest::qWaitFor([&] { return window.shareEngine()->isRunning(); }, 10000)) << window.shareEngine()->error().toStdString();
}

// 9. Nothing stored, but something asks (Share this note, Join): the module loads then, and not before.
TEST(LazyModules, ShareLoadsWhenAskedFor)
{
    LazyVault vault;
    QTemporaryDir store;
    MainWindow window;
    configureShare(window, store.path());
    startAndScan(window, vault.root());
    ASSERT_EQ(window.shareEngine(), nullptr);
    QString error;
    ASSERT_TRUE(window.ensureShare(&error)) << error.toStdString();
    ASSERT_NE(window.shareEngine(), nullptr);
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("share")));
    EXPECT_FALSE(loadedObjectsMatching(QStringLiteral("libprotobuf")).isEmpty());
    EXPECT_FALSE(window.shareEngine()->isRunning()) << "built, not started: no keys read, no socket";
}

// 10. Asked for before any scan has run (the command is the first thing a session does): it scans itself.
TEST(LazyModules, ShareAskedForBeforeAnyScan)
{
    LazyVault vault;
    QTemporaryDir store;
    MainWindow window;
    configureShare(window, store.path());
    window.show();
    window.openTarget(resolveLaunchTarget(vault.root()));
    QString error;
    ASSERT_TRUE(window.ensureShare(&error)) << error.toStdString();
    EXPECT_NE(window.shareEngine(), nullptr);
}

// 11. The feature is off: nothing is built and nothing is mapped, even when something asks.
TEST(LazyModules, ShareOffMapsNothing)
{
    QTemporaryDir config; // the switch is stored: not in the home the other entries share
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    setShareFeature(false);
    LazyVault vault;
    QTemporaryDir store;
    MainWindow window;
    configureShare(window, store.path());
    startAndScan(window, vault.root());
    QString error;
    EXPECT_FALSE(window.ensureShare(&error));
    EXPECT_TRUE(error.contains(QStringLiteral("Share a note"))) << error.toStdString();
    EXPECT_EQ(window.shareEngine(), nullptr);
    EXPECT_FALSE(anyMapped(kShareStack));
}

// 12. No module installed (the ctest entry points the registry at a directory that is not there).
TEST(LazyModules, ShareWithoutTheModuleSaysWhatIsMissing)
{
    LazyVault vault;
    QTemporaryDir store;
    MainWindow window;
    configureShare(window, store.path());
    window.show();
    window.openTarget(resolveLaunchTarget(vault.root()));
    window.scanModulesNow();
    QString error;
    EXPECT_FALSE(window.ensureShare(&error));
    auto *toggle = featureToggle(window, "share");
    ASSERT_NE(toggle, nullptr) << "the Share a note row vanished";
    EXPECT_FALSE(toggle->isEnabled());
    EXPECT_TRUE(error.contains(QStringLiteral("notolith-share"))) << error.toStdString();
    EXPECT_EQ(window.shareEngine(), nullptr);
    EXPECT_FALSE(anyMapped(kShareStack));
}
#endif // NOTEAPPC_HAVE_SHARE

// 13. The last session ended with a PDF open: the restored tab opens it although no module scan has run yet,
// and the saved workspace keeps it.
TEST(LazyModules, PdfTabRestoredFromTheWorkspaceOpens)
{
    LazyVault vault;
    seedWorkspaceWith(vault.root(), QStringLiteral("Paper.pdf"));
    {
        MainWindow window;
        window.show();
        window.openTarget(resolveLaunchTarget(vault.root()));
        EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("pdfview"))) << "the tab was dropped";
        EXPECT_EQ(window.findChildren<ModuleView *>().size(), 1);
        window.close();
    }
    EXPECT_TRUE(workspaceNames(vault.root(), QStringLiteral("Paper.pdf"))) << "the workspace lost the tab";
}

// 14. The same with no module installed (the ctest entry points the registry at a directory that is not there):
// nothing opens, and the workspace still names the file, for the day the package is installed.
TEST(LazyModules, PdfTabIsKeptInTheWorkspaceWithoutTheModule)
{
    LazyVault vault;
    seedWorkspaceWith(vault.root(), QStringLiteral("Paper.pdf"));
    {
        MainWindow window;
        window.show();
        window.openTarget(resolveLaunchTarget(vault.root()));
        EXPECT_EQ(window.findChildren<ModuleView *>().size(), 0);
        window.close();
    }
    EXPECT_TRUE(workspaceNames(vault.root(), QStringLiteral("Paper.pdf"))) << "the workspace lost the tab";
}

// Focus is not a retry: a kept tab that is focused again and again says nothing, leaves Settings alone, and a note
// opened into it ends the keeping.
TEST(LazyModules, KeptPdfTabIgnoresFocusAndEndsWhenANoteOpensInIt)
{
    LazyVault vault;
    seedWorkspaceWith(vault.root(), QStringLiteral("Paper.pdf"));
    {
        MainWindow window;
        window.show();
        window.openTarget(resolveLaunchTarget(vault.root()));
        window.scanModulesNow(); // no module: nothing to wait for
        QApplication::processEvents();
        ASSERT_EQ(window.findChildren<ModuleView *>().size(), 0);
        int messages = 0;
        QObject::connect(window.statusBar(), &QStatusBar::messageChanged, [&messages](const QString &text) {
            if (!text.isEmpty())
                ++messages;
        });
        window.openSettings();
        ASSERT_TRUE(window.settingsOpen());
        const auto toggle = featureToggle(window, "pdf-embeds");
        ASSERT_NE(toggle, nullptr);
        EXPECT_FALSE(toggle->isChecked()) << "an unavailable feature shows off";
        EXPECT_FALSE(toggle->isEnabled());
        ASSERT_TRUE(window.settingsOpen());
        messages = 0;
        for (int round = 0; round < 5; ++round) {
            QFocusEvent out(QEvent::FocusOut);
            QApplication::sendEvent(window.editor(), &out);
            QFocusEvent in(QEvent::FocusIn);
            QApplication::sendEvent(window.editor(), &in);
        }
        EXPECT_EQ(messages, 0) << "focus retried the kept PDF";
        EXPECT_TRUE(window.settingsOpen()) << "focus closed Settings";
        ASSERT_TRUE(window.openNote(vault.notePath()));
        QFocusEvent in(QEvent::FocusIn);
        QApplication::sendEvent(window.editor(), &in);
        EXPECT_EQ(messages, 0);
        window.close();
    }
    EXPECT_TRUE(workspaceNames(vault.root(), QStringLiteral("Note.md")));
    EXPECT_FALSE(workspaceNames(vault.root(), QStringLiteral("Paper.pdf"))) << "the opened note did not end the keeping";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef NOTEAPPC_HAVE_SHARE
    // Never the real keyring (a silent secret-service costs 15 s a call).
    static QHash<QString, QByteArray> memory;
    noteAppC::secrets::setStore({[](const QString &key, QByteArray *value) {
                                     if (!memory.contains(key))
                                         return false;
                                     *value = memory.value(key);
                                     return true;
                                 },
                                 [](const QString &key, const QByteArray &value) { return memory.insert(key, value) != memory.end(); },
                                 [](const QString &key) { return memory.remove(key) > 0; }});
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
