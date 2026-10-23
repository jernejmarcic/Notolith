// What a start maps, and what a module costs until it is wanted (lane F): poppler and the share stack live in
// loadable modules (libs/ext/pdfview, libs/ext/shareengine), so a session that never meets a PDF, and one that never
// shares, maps none of their libraries. Each test is its own ctest entry (tests/CMakeLists.txt): what is mapped is a
// fact about the whole process, and an earlier test in it would already have mapped what a later one asserts absent.
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPdfWriter>
#include <QPainter>
#include <QTemporaryDir>

#include "LoadedObjects.h"
#include "MainWindow.h"
#include "ModuleHost.h"
#include "ModuleRegistry.h"
#include "PdfPage.h"
#include "Settings.h"
#include "WaitFor.h"

using namespace noteAppC;

namespace {

const QStringList kPoppler = {QStringLiteral("libpoppler")};

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

/// Starts the way main() does (openTarget runs after the first paint) and waits for the module scan that follows it.
void startAndScan(MainWindow &window, const QString &vaultRoot)
{
    window.show();
    window.openTarget(resolveLaunchTarget(vaultRoot));
    QApplication::processEvents();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window.modules()->modules().isEmpty(); }, 10000)) << "no module scan result";
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
    EXPECT_FALSE(anyMapped(kPoppler));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
