// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// An .ink note in the index: its body is its text boxes, also after a rename rewrote the links inside it.
// Links the host (Indexer, Index, InkTextBoxes) with the inkcore reader, to check the file still opens.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "InkDocument.h"
#include "InkFormat.h"
#include "InkTextBoxes.h"
#include "Index.h"
#include "Indexer.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

class InkIndexTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        write(QStringLiteral("Welcome.md"), "# Welcome\n");
        ink::InkDocument document;
        ink::Item item;
        item.type = ink::Item::Type::TextBox;
        item.textBox = std::make_shared<ink::TextBox>(ink::TextBox{QRectF(10, 10, 300, 40), QStringLiteral("see [[Welcome]]")});
        document.addItem(item);
        write(QStringLiteral("Sketch.ink"), ink::writeInk(document));
        vault = Vault(dir.path());
        indexer = std::make_unique<Indexer>(&index);
        indexer->setForeignReader({QStringLiteral("ink")}, inkTextBoxes, kInkIndexMaxBytes);
        QSignalSpy done(indexer.get(), &Indexer::finished);
        indexer->setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }

    void write(const QString &relative, const QByteArray &bytes)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    /// The file's one text box, read by inkcore (what the module opens).
    QString textBoxOf(const QString &relative)
    {
        ink::InkDocument document;
        ink::InkReadResult result;
        EXPECT_TRUE(ink::loadInkFile(dir.filePath(relative), document, &result)) << qPrintable(result.error);
        for (const ink::Item &item : document.items())
            if (item.textBox)
                return item.textBox->markdown;
        return {};
    }

    void rename(const QString &from, const QString &to)
    {
        ASSERT_TRUE(QFile::rename(dir.filePath(from), dir.filePath(to)));
        QString error;
        ASSERT_GE(index.updateLinksAfterRename(from, to, vault, &error), 0) << qPrintable(error);
    }

    bool searchFinds(const QString &word, const QString &path)
    {
        for (const auto &hit : index.search(word))
            if (hit.path == path)
                return true;
        return false;
    }

    QTemporaryDir dir;
    Vault vault;
    Index index;
    std::unique_ptr<Indexer> indexer;
};

} // namespace

TEST_F(InkIndexTest, RenamingALinkedNoteKeepsTheInkNoteIndexedAsItsTextAndOpenable)
{
    ASSERT_TRUE(searchFinds(QStringLiteral("see"), QStringLiteral("Sketch.ink")));
    rename(QStringLiteral("Welcome.md"), QStringLiteral("Hello.md"));
    EXPECT_EQ(textBoxOf(QStringLiteral("Sketch.ink")), QStringLiteral("see [[Hello]]")) << "the link follows, in a file inkcore still reads";
    EXPECT_TRUE(searchFinds(QStringLiteral("see"), QStringLiteral("Sketch.ink")));
    EXPECT_FALSE(searchFinds(QStringLiteral("textbox"), QStringLiteral("Sketch.ink"))) << "the body is the text, not the JSON";
}

TEST_F(InkIndexTest, ANewNameWithAQuoteAndABackslashKeepsTheInkFileValid)
{
    const QString name = QStringLiteral("Say \"hi\" \\ bye");
    rename(QStringLiteral("Welcome.md"), name + QStringLiteral(".md"));
    EXPECT_EQ(textBoxOf(QStringLiteral("Sketch.ink")), QStringLiteral("see [[") + name + QStringLiteral("]]"));
    EXPECT_FALSE(searchFinds(QStringLiteral("textbox"), QStringLiteral("Sketch.ink")));
}

TEST_F(InkIndexTest, RenamingTheInkNoteItselfKeepsItsRowTheText)
{
    rename(QStringLiteral("Sketch.ink"), QStringLiteral("Drawing.ink"));
    EXPECT_TRUE(searchFinds(QStringLiteral("see"), QStringLiteral("Drawing.ink")));
    EXPECT_FALSE(searchFinds(QStringLiteral("textbox"), QStringLiteral("Drawing.ink")));
    EXPECT_FALSE(searchFinds(QStringLiteral("see"), QStringLiteral("Sketch.ink"))) << "the old row is gone";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
