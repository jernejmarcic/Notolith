// InkView without dlopen (the static noteAppC_ink linked in, as Track S1's tests do): what the tab does
// about a file that changes under it (the vault is Syncthing-synced) and about a newer file's read-only
// state. The window-level behaviour of the same view is in test_window.cpp.
#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "InkView.h"
#include "core/InkFormat.h"

using namespace noteAppC::ink;

namespace {

std::shared_ptr<const Stroke> line(double x0, double y0, double x1, double y1)
{
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = 2.0;
    stroke->points = {{x0, y0, 0.5}, {x1, y1, 0.5}};
    return stroke;
}

QByteArray documentWith(int strokes)
{
    InkDocument doc;
    for (int i = 0; i < strokes; ++i)
        doc.addStroke(line(10 + i, 10, 50 + i, 50));
    doc.commit();
    return writeInk(doc);
}

/// What another machine's sync does: replaces the bytes and gives the file a later modification time.
void replaceExternally(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList conflictFiles(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("Sketch (conflict *).ink")}, QDir::Files);
}

class InkViewFileTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        path = dir.filePath(QStringLiteral("Sketch.ink"));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(documentWith(1));
    }
    QTemporaryDir dir;
    QString path;
};

} // namespace

TEST_F(InkViewFileTest, ANewerFileIsReadOnlyFlaggedAnnouncedAndNeverWritten)
{
    const QByteArray newer = "{\"format\":\"notolith-ink\",\"version\":2,\"document\":{},\"items\":[]}\n";
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(newer);
    }
    InkView view(nullptr);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    EXPECT_TRUE(view.isReadOnly());
    EXPECT_TRUE(view.property("readOnly").toBool()) << "the flag a test without the class can read";
    ASSERT_EQ(status.count(), 1);
    EXPECT_TRUE(status.first().first().toString().contains(QStringLiteral("read-only")));
    view.show();
    auto *label = view.findChild<QLabel *>(QStringLiteral("inkReadOnlyLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->isHidden()) << "a persistent indicator, not only a status message";
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkPenAction"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkEraserAction"))->isEnabled());
    EXPECT_FALSE(view.save(&error));
    EXPECT_EQ(readAll(path), newer);
}

TEST_F(InkViewFileTest, AnExternalChangeWithoutLocalEditsIsReloadedNotOverwritten)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    ASSERT_EQ(view.canvas()->document().items().size(), 1u);
    const QByteArray theirs = documentWith(3);
    replaceExternally(path, theirs);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    EXPECT_TRUE(view.save(&error)) << error.toStdString();
    EXPECT_EQ(readAll(path), theirs) << "their file is untouched";
    EXPECT_EQ(view.canvas()->document().items().size(), 3u) << "the view shows what is on disk now";
    EXPECT_FALSE(view.isModified());
    EXPECT_TRUE(conflictFiles(dir.path()).isEmpty());
    ASSERT_GE(status.count(), 1);
    EXPECT_TRUE(status.last().first().toString().contains(QStringLiteral("changed on disk")));
}

TEST_F(InkViewFileTest, AnExternalChangeWithLocalEditsKeepsBothVersions)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    view.canvas()->document().addStroke(line(100, 100, 200, 200));
    view.canvas()->document().commit();
    ASSERT_TRUE(view.isModified());
    const QByteArray theirs = documentWith(3);
    replaceExternally(path, theirs);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    EXPECT_TRUE(view.save(&error)) << error.toStdString();
    EXPECT_EQ(readAll(path), theirs) << "the original on disk is untouched";
    const QStringList conflicts = conflictFiles(dir.path());
    ASSERT_EQ(conflicts.size(), 1);
    InkDocument mine;
    ASSERT_TRUE(readInk(readAll(dir.filePath(conflicts.first())), mine).ok);
    EXPECT_EQ(mine.items().size(), 2u) << "the conflict file holds the local document: the old stroke and the new one";
    EXPECT_EQ(view.canvas()->document().items().size(), 3u) << "the view now shows the disk version";
    EXPECT_FALSE(view.isModified());
    ASSERT_GE(status.count(), 1);
    EXPECT_TRUE(status.last().first().toString().contains(conflicts.first())) << status.last().first().toString().toStdString();
}

TEST_F(InkViewFileTest, OwnSavesAreNotConflicts)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    for (int i = 0; i < 3; ++i) {
        view.canvas()->document().addStroke(line(20 * i, 5, 20 * i + 10, 60));
        view.canvas()->document().commit();
        ASSERT_TRUE(view.save(&error)) << error.toStdString();
    }
    EXPECT_TRUE(conflictFiles(dir.path()).isEmpty());
    InkDocument onDisk;
    ASSERT_TRUE(readInk(readAll(path), onDisk).ok);
    EXPECT_EQ(onDisk.items().size(), 4u);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
