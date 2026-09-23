// Regression test for demo/: opens the whole demo vault in a real MainWindow
// and walks every note, canvas and base file, painting each so a parser or
// layout crash shows up as a test failure instead of something a person has
// to notice by eye. See demo/Welcome.md for what the vault covers and
// demo-assets.md (repo root) for where its binary assets came from.
//
// NOTEAPPC_DEMO_VAULT_PATH is the absolute path to demo/, set by
// tests/CMakeLists.txt so this does not depend on the working directory a
// test runner happens to use.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>

#include "Index.h"
#include "MainWindow.h"
#include "MarkdownEditor.h"

using noteAppC::Index;
using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;

namespace {

QString vaultPath()
{
    return QStringLiteral(NOTEAPPC_DEMO_VAULT_PATH);
}

// Every note, canvas and base file under the vault, vault-relative, sorted -
// deterministic order for a reproducible failure. .obsidian's own JSON is
// vault configuration, not something MainWindow::openNote ever opens, so it
// is not walked here.
QStringList demoFiles()
{
    QStringList files;
    QDirIterator it(vaultPath(), {QStringLiteral("*.md"), QStringLiteral("*.canvas"), QStringLiteral("*.base")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    std::sort(files.begin(), files.end());
    return files;
}

// du -sb equivalent: every byte under demo/, hidden files included, the same
// total the "under 5 MB" rule is measured against.
qint64 totalVaultBytes()
{
    qint64 total = 0;
    QDirIterator it(vaultPath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

} // namespace

TEST(DemoVaultTest, StaysUnderTheFiveMegabyteBudget)
{
    ASSERT_TRUE(QFileInfo::exists(vaultPath())) << vaultPath().toStdString();
    const qint64 bytes = totalVaultBytes();
    EXPECT_LT(bytes, qint64(5) * 1024 * 1024)
        << bytes << " bytes - see demo-assets.md before adding anything larger.";
}

TEST(DemoVaultTest, HasContentToWalk)
{
    // A guard against the file-listing logic itself silently finding
    // nothing and the walk test below passing for the wrong reason.
    ASSERT_GT(demoFiles().size(), 50);
}

TEST(DemoVaultTest, EveryFileOpensAndPaintsInEveryMode)
{
    QElapsedTimer clock;
    clock.start();

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(vaultPath());
    ASSERT_TRUE(indexed.wait(15000)) << "index never finished";
    window.resize(1280, 900);
    window.show();
    QApplication::processEvents();

    const QStringList files = demoFiles();
    ASSERT_FALSE(files.isEmpty());

    for (const QString &path : files) {
        SCOPED_TRACE(path.toStdString());

        ASSERT_TRUE(window.openNote(path)) << "failed to open " << path.toStdString();
        QApplication::processEvents();

        const QImage livePreview = window.grab().toImage();
        EXPECT_FALSE(livePreview.isNull());

        // Reading and live-preview mode only mean anything for a Markdown
        // note - a canvas or base file has no editor mode to switch. Set the
        // mode directly rather than through the "markdown:toggle-preview"
        // command/Ctrl+E: that command only flips Source<->LivePreview and
        // never reaches Mode::Reading at all (see the report for details),
        // so it cannot be used to reach reading mode here.
        if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) {
            MarkdownEditor *editor = window.editor();
            ASSERT_NE(editor, nullptr);

            editor->setMode(MarkdownEditor::Mode::Reading);
            QApplication::processEvents();
            const QImage reading = window.grab().toImage();
            EXPECT_FALSE(reading.isNull());

            editor->setMode(MarkdownEditor::Mode::LivePreview);
            QApplication::processEvents();
        }
    }

    // "well under a minute" for the whole walk.
    EXPECT_LT(clock.elapsed(), 60000);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings::configDirectory() is fixed to ~/.config/noteAppC regardless
    // of QCoreApplication's org/app name, so every test that builds a
    // MainWindow needs its own XDG_CONFIG_HOME - otherwise this would read
    // and could write whoever runs it their real settings file.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
