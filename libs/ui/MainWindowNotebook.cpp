// SPDX-License-Identifier: GPL-3.0-or-later
// A Jupyter notebook's cell view on the window's side (docs/NOTEBOOKS.md, "Cell view"): the toolbar over a
// notebook's editor and what its buttons do to the cells. The cells are the text's own (notebook::ipynbCells):
// an action rebuilds the text from the changed list and the editor applies the difference as one undo step, so
// saving, undo and every other view of the note work as for typing.

#include "MainWindow.h"

#include "Icons.h"
#include "Ipynb.h"
#include "KernelSpec.h"
#include "NotebookController.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace noteAppC {

namespace {

using notebook::IpynbCell;

/// The cell holding `offset`: the last one starting at or before it, else the first; -1 with none.
int cellIndexAt(const QList<IpynbCell> &cells, qsizetype offset)
{
    int index = cells.isEmpty() ? -1 : 0;
    for (int i = 0; i < cells.size(); ++i) {
        if (cells.at(i).start <= offset)
            index = i;
    }
    return index;
}

/// Where the caret goes in `cell` of `text`: inside a code cell's fence, at the start of a markdown one.
qsizetype caretInside(const QString &text, const IpynbCell &cell)
{
    if (cell.type == IpynbCell::Type::Markdown)
        return cell.start;
    const qsizetype newline = text.indexOf(QLatin1Char('\n'), cell.start);
    return newline < 0 ? text.size() : newline + 1;
}

} // namespace

void MainWindow::syncNotebookBar(Leaf *leaf)
{
    if (!leaf)
        return;
    const bool notebook = !leaf->view && isNotebookFile(leaf->notePath);
    leaf->editor->setNotebookLayout(notebook);
    if (!notebook) {
        if (leaf->notebookBar)
            leaf->notebookBar->hide();
        return;
    }
    if (!leaf->notebookBar) {
        auto *bar = new QWidget(leaf->area);
        bar->setObjectName(QStringLiteral("notebookBar"));
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(10, 4, 10, 4);
        row->setSpacing(2);
        const auto button = [&](const char *name, const char *icon, const QString &tip, auto &&action) {
            QToolButton *b = iconButton(QString::fromLatin1(name), QString::fromLatin1(icon), tip, 16);
            b->setParent(bar);
            updateTabButtonIcon(b);
            connect(b, &QToolButton::clicked, this, action);
            row->addWidget(b);
        };
        const auto gap = [&] { row->addSpacing(10); };
        button("notebookAddCell", "plus", tr("Insert a cell below (B)"),
               [this] { editNotebookCells([](QList<IpynbCell> &cells, int at) {
                   cells.insert(at + 1, IpynbCell{IpynbCell::Type::Code, QString(), -1});
                   return at + 1;
               }); });
        gap();
        button("notebookCutCell", "scissors", tr("Cut the cell"), [this] {
            editNotebookCells([this](QList<IpynbCell> &cells, int at) {
                if (at < 0)
                    return at;
                m_notebookCellClipboard = std::make_shared<IpynbCell>(cells.takeAt(at));
                return qMin(at, int(cells.size()) - 1);
            });
        });
        button("notebookCopyCell", "copy", tr("Copy the cell"), [this] {
            editNotebookCells([this](QList<IpynbCell> &cells, int at) {
                if (at >= 0)
                    m_notebookCellClipboard = std::make_shared<IpynbCell>(cells.at(at));
                return at;
            });
        });
        button("notebookPasteCell", "clipboard-paste", tr("Paste the cell below"), [this] {
            editNotebookCells([this](QList<IpynbCell> &cells, int at) {
                if (!m_notebookCellClipboard)
                    return at;
                cells.insert(at + 1, *m_notebookCellClipboard);
                return at + 1;
            });
        });
        gap();
        button("notebookMoveUp", "arrow-up", tr("Move the cell up"), [this] {
            editNotebookCells([](QList<IpynbCell> &cells, int at) {
                if (at <= 0)
                    return at;
                cells.swapItemsAt(at, at - 1);
                return at - 1;
            });
        });
        button("notebookMoveDown", "arrow-down", tr("Move the cell down"), [this] {
            editNotebookCells([](QList<IpynbCell> &cells, int at) {
                if (at < 0 || at + 1 >= cells.size())
                    return at;
                cells.swapItemsAt(at, at + 1);
                return at + 1;
            });
        });
        gap();
        button("notebookRunCell", "play", tr("Run the cell and go to the next (Shift+Enter)"),
               [this] { runNotebookCellAndAdvance(); });
        button("notebookInterrupt", "square", tr("Interrupt the kernel"), [this] {
            if (m_notebook && !m_active->view)
                m_notebook->interruptKernel(m_active->editor->codeLanguageAt(m_active->editor->cursorOffset()),
                                            m_active->notePath);
        });
        button("notebookRestart", "rotate-ccw", tr("Restart the kernel"), [this] {
            if (m_notebook && !m_active->view)
                m_notebook->restartKernel(QString(), m_active->notePath);
        });
        button("notebookRunAll", "fast-forward", tr("Restart the kernel and run every cell"), [this] {
            if (m_active->view)
                return;
            if (m_notebook)
                m_notebook->restartKernel(QString(), m_active->notePath);
            m_active->editor->runAllNotebookBlocks();
        });
        gap();
        auto *type = new QComboBox(bar);
        type->setObjectName(QStringLiteral("notebookCellType"));
        type->addItem(tr("Code"), int(IpynbCell::Type::Code));
        type->addItem(tr("Markdown"), int(IpynbCell::Type::Markdown));
        type->addItem(tr("Raw"), int(IpynbCell::Type::Raw));
        type->setFocusPolicy(Qt::NoFocus);
        connect(type, &QComboBox::activated, this, [this, type](int index) {
            const auto chosen = IpynbCell::Type(type->itemData(index).toInt());
            editNotebookCells([chosen](QList<IpynbCell> &cells, int at) {
                if (at >= 0)
                    cells[at].type = chosen;
                return at;
            });
        });
        row->addWidget(type);
        row->addStretch(1);
        auto *kernel = new QLabel(bar);
        kernel->setObjectName(QStringLiteral("notebookKernel"));
        row->addWidget(kernel);
        static_cast<QVBoxLayout *>(leaf->area->layout())->insertWidget(0, bar);
        leaf->notebookBar = bar;
        connect(leaf->editor, &MarkdownEditor::notebookRunAndAdvance, this, [this] { runNotebookCellAndAdvance(); });
        connect(leaf->editor, &MarkdownEditor::cursorMoved, this, [this, leaf] {
            if (leaf->notebookBar && leaf->notebookBar->isVisible())
                m_notebookBarTimer.start();
        });
    }
    leaf->notebookBar->show();
    const NotebookInfo info = m_notebookInfo.value(leaf->notePath);
    leaf->notebookBar->findChild<QLabel *>(QStringLiteral("notebookKernel"))
        ->setText(info.kernel.isEmpty() ? info.language : info.kernel);
    syncNotebookCellType(leaf);
}

void MainWindow::syncNotebookCellType(Leaf *leaf)
{
    if (!leaf || !leaf->notebookBar || !leaf->notebookBar->isVisible())
        return;
    const QString text = leaf->editor->noteText();
    const QList<IpynbCell> cells = notebook::ipynbCells(text, notebookLanguageOf(leaf->notePath));
    const int at = cellIndexAt(cells, leaf->editor->cursorOffset());
    auto *type = leaf->notebookBar->findChild<QComboBox *>(QStringLiteral("notebookCellType"));
    type->setCurrentIndex(type->findData(int(at < 0 ? IpynbCell::Type::Code : cells.at(at).type)));
}

QString MainWindow::notebookLanguageOf(const QString &path) const
{
    const QString language = m_notebookInfo.value(path).language;
    return language.isEmpty() ? QStringLiteral("python") : language;
}

void MainWindow::editNotebookCells(const std::function<int(QList<IpynbCell> &, int)> &change)
{
    Leaf *leaf = m_active;
    if (!leaf || leaf->view || !isNotebookFile(leaf->notePath))
        return;
    const QString language = notebookLanguageOf(leaf->notePath);
    const QString text = leaf->editor->noteText();
    QList<IpynbCell> cells = notebook::ipynbCells(text, language);
    const int next = change(cells, cellIndexAt(cells, leaf->editor->cursorOffset()));
    const QString changed = notebook::ipynbText(cells, language);
    const QList<IpynbCell> placed = notebook::ipynbCells(changed, language);
    const qsizetype caret = next >= 0 && next < placed.size() ? caretInside(changed, placed.at(next)) : changed.size();
    if (changed == text) {
        leaf->editor->setCursorOffset(caret);
    } else {
        const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(text, changed);
        leaf->editor->replaceAsOneStep(hunk.at, hunk.removed, hunk.inserted, caret);
    }
    leaf->editor->setFocus();
    syncNotebookCellType(leaf);
}

void MainWindow::runNotebookCellAndAdvance()
{
    Leaf *leaf = m_active;
    if (!leaf || leaf->view || !isNotebookFile(leaf->notePath))
        return;
    const QList<IpynbCell> cells = notebook::ipynbCells(leaf->editor->noteText(), notebookLanguageOf(leaf->notePath));
    const int at = cellIndexAt(cells, leaf->editor->cursorOffset());
    if (at >= 0 && cells.at(at).type == IpynbCell::Type::Code)
        leaf->editor->runNotebookBlockAt(cells.at(at).start);
    // Jupyter's Shift+Enter: on to the next cell, a new code cell after the last.
    editNotebookCells([](QList<IpynbCell> &list, int current) {
        if (current + 1 >= list.size())
            list.append(IpynbCell{IpynbCell::Type::Code, QString(), -1});
        return current + 1;
    });
}

} // namespace noteAppC
