// SPDX-License-Identifier: GPL-3.0-or-later
// Seeded round-trip fuzz for the two promises everything else leans on:
// the file is the truth (a note read and written back is the same bytes),
// and the editor's incremental keystroke path (applyEdit, the shifted
// decorations, the line map) always holds exactly the text a plain string
// would after the same edits. Deterministic: a failure names its seed.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QScrollBar>
#include <QTest>

#include "MarkdownEditor.h"
#include "Vault.h"

using noteAppC::MarkdownEditor;
using noteAppC::Note;
using noteAppC::Vault;

namespace {

/// A note built from the constructs the editor treats specially, in random
/// order and amount. BMP characters only: the editor's Delete removes one
/// UTF-16 unit, and a plain QString model does the same.
QString randomNote(QRandomGenerator &rng)
{
    static const QStringList pieces{
        QStringLiteral("# Heading\n"),
        QStringLiteral("## Sub *heading* %1\n"),
        QStringLiteral("A paragraph with **bold**, *italic*, ==mark== and `code` %1.\n"),
        QStringLiteral("See [[Note %1]] and [[Other|shown]] and [a](b.md).\n"),
        QStringLiteral("- item %1\n  - nested\n- [ ] task\n- [x] done\n"),
        QStringLiteral("1. one\n2. two\n"),
        QStringLiteral("```cpp\nint x = %1;\n// comment\n```\n"),
        QStringLiteral("> [!note] Title\n> body %1\n"),
        QStringLiteral("| a | b |\n|---|---|\n| 1 | %1 |\n"),
        QStringLiteral("$$\nx^%1\n$$\n"),
        QStringLiteral("Inline $a+b$ maths and a #tag/%1.\n"),
        QStringLiteral("<div>html %1</div>\n"),
        QStringLiteral("Čšž ünïcode 漢字 %1\n"),
        QStringLiteral("\n"),
        QStringLiteral("---\n"),
    };
    QString text;
    if (rng.bounded(3) == 0)
        text += QStringLiteral("---\ntitle: T\ntags: [a, b]\n---\n");
    const int count = 1 + int(rng.bounded(12));
    for (int i = 0; i < count; ++i)
        text += pieces.at(int(rng.bounded(qsizetype(pieces.size())))).arg(i);
    return text;
}

} // namespace

TEST(FuzzTest, ANoteReadAndWrittenBackIsTheSameBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (quint32 seed = 1; seed <= 150; ++seed) {
        QRandomGenerator rng(seed);
        QByteArray bytes = randomNote(rng).toUtf8();
        if (seed % 3 == 0)
            bytes.replace("\n", "\r\n");
        if (seed % 5 == 0)
            bytes.prepend("\xEF\xBB\xBF");
        const QString in = QDir(dir.path()).filePath(QStringLiteral("in.md"));
        const QString out = QDir(dir.path()).filePath(QStringLiteral("out.md"));
        QFile file(in);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
        file.close();

        Note note;
        ASSERT_TRUE(Vault::read(in, note)) << "seed " << seed;
        ASSERT_TRUE(Vault::write(out, note)) << "seed " << seed;
        QFile written(out);
        ASSERT_TRUE(written.open(QIODevice::ReadOnly));
        EXPECT_EQ(written.readAll(), bytes) << "seed " << seed;
    }
}

TEST(FuzzTest, RandomEditsKeepTheEditorsTextAndUndoPutsItBack)
{
    static const QStringList typed{
        QStringLiteral("a"), QStringLiteral(" "), QStringLiteral("\n"), QStringLiteral("*"),
        QStringLiteral("`"), QStringLiteral("["), QStringLiteral("]"), QStringLiteral("#"),
        QStringLiteral("- "), QStringLiteral("> "), QStringLiteral("|"), QStringLiteral("$"),
        QStringLiteral("```"), QStringLiteral("\n\n"), QStringLiteral("[["), QStringLiteral("ž"),
    };
    for (quint32 seed = 1; seed <= 24; ++seed) {
        QRandomGenerator rng(seed);
        const QString original = randomNote(rng);
        MarkdownEditor editor;
        editor.resize(700, 500);
        editor.setAlignTables(false); // the raw keystroke path; aligning is TableFormatEditorTest's
        editor.setNoteText(original);
        QString model = original;

        const int edits = 50;
        for (int step = 0; step < edits; ++step) {
            const qsizetype at = qsizetype(rng.bounded(quint32(model.size() + 1)));
            editor.setCursorOffset(at);
            if (rng.bounded(3) == 0 && at < model.size()) {
                const QString before = editor.noteText();
                QTest::keyClick(&editor, Qt::Key_Delete);
                // The one Delete that deletes nothing by design: at the end of
                // a drawn table's cell, where it would eat the pipe.
                if (editor.noteText() == before && editor.tableRenderedAt(at))
                    continue;
                model.remove(at, 1);
            } else {
                const QString text = typed.at(int(rng.bounded(qsizetype(typed.size()))));
                editor.insertAtCaret(text);
                model.insert(at, text);
            }
            ASSERT_EQ(editor.noteText(), model) << "seed " << seed << ", edit " << step;
            if (step % 10 == 0)
                (void)editor.grab(); // lay out and paint what the edits left
        }
        // Once the reparse after the last edit has landed, fifty edits leave on
        // screen what a fresh load of the same text draws: a cache or a code
        // highlighter state that survives a reparse shows as a different
        // pixel. (Before it lands they may differ by design: a keystroke
        // styles its own block, and a ``` typed there changes the rest of
        // the note only when the whole note is parsed again.)
        MarkdownEditor fresh;
        fresh.resize(editor.size());
        fresh.setNoteText(model);
        fresh.setCursorOffset(editor.cursorOffset());
        // Typing styles the edited block at once and reparses the note 20 ms
        // after the last keystroke, off the UI thread; compare after that.
        QTest::qWait(120);
        // Reading view: no caret, and no block revealed around it, which the
        // edited editor has and a fresh one does not.
        editor.setMode(MarkdownEditor::Mode::Reading);
        fresh.setMode(MarkdownEditor::Mode::Reading);
        fresh.verticalScrollBar()->setValue(editor.verticalScrollBar()->value());
        // Both lay out the same way first: blocks off screen are only
        // estimated, and the scrollbar's thumb is sized from the heights
        // known so far. Lines that break at every newline make a note taller
        // than the part laid out at once, and a thumb a line off is not a
        // pixel of text differing.
        (void)editor.blockBoxes();
        (void)fresh.blockBoxes();
        EXPECT_EQ(editor.grab().toImage(), fresh.grab().toImage()) << "seed " << seed;

        editor.setMode(MarkdownEditor::Mode::LivePreview);
        for (int step = 0; step < edits; ++step)
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        EXPECT_EQ(editor.noteText(), original) << "seed " << seed;
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
