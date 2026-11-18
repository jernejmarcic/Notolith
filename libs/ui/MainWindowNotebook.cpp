// SPDX-License-Identifier: GPL-3.0-or-later
// A Jupyter notebook's cell view on the window's side (docs/NOTEBOOKS.md, "Cell view"): the toolbar over a
// notebook's editor and what its buttons do to the cells. The cells are the text's own (notebook::ipynbCells):
// an action rebuilds the text from the changed list and the editor applies the difference as one undo step, so
// saving, undo and every other view of the note work as for typing. The sidebar's Notebook pane edits the
// caret cell's tags and metadata, held with the runs until the notebook is saved.

#include "MainWindow.h"

#include "Icons.h"
#include "Ipynb.h"
#include "KernelSpec.h"
#include "NotebookController.h"
#include "Vault.h"

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
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
    syncNotebookPane();
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

QWidget *MainWindow::buildNotebookPane(QWidget *parent)
{
    auto *pane = new QWidget(parent);
    pane->setObjectName(QStringLiteral("notebookPane"));
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    const auto label = [pane](const char *name) {
        auto *l = new QLabel(pane);
        l->setObjectName(QLatin1String(name));
        l->setWordWrap(true);
        return l;
    };
    layout->addWidget(label("notebookPaneKernel"));
    layout->addWidget(label("notebookPaneCell"));
    auto *form = new QWidget(pane);
    form->setObjectName(QStringLiteral("notebookPaneCellForm"));
    auto *fields = new QVBoxLayout(form);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->addWidget(new QLabel(tr("Tags"), form));
    auto *tags = new QLineEdit(form);
    tags->setObjectName(QStringLiteral("notebookPaneTags"));
    tags->setPlaceholderText(tr("tag, another tag"));
    fields->addWidget(tags);
    fields->addWidget(new QLabel(tr("Metadata (JSON)"), form));
    auto *json = new QPlainTextEdit(form);
    json->setObjectName(QStringLiteral("notebookPaneMetadata"));
    json->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    fields->addWidget(json, 1);
    auto *apply = new QPushButton(tr("Apply"), form);
    apply->setObjectName(QStringLiteral("notebookPaneApply"));
    fields->addWidget(apply, 0, Qt::AlignRight);
    fields->addWidget(label("notebookPaneStatus"));
    layout->addWidget(form, 1);
    layout->addStretch(0);
    connect(apply, &QPushButton::clicked, this, [this] { applyNotebookCellMetadata(); });
    connect(tags, &QLineEdit::returnPressed, this, [this] { applyNotebookCellMetadata(); });
    m_notebookPane = pane;
    return pane;
}

void MainWindow::syncNotebookPane()
{
    QWidget *pane = m_notebookPane;
    if (!pane)
        return;
    // What is being typed into the pane stays until it is applied or the editor has the caret again.
    if (QWidget *focus = QApplication::focusWidget(); focus && pane->isAncestorOf(focus))
        return;
    const auto label = [pane](const char *name) { return pane->findChild<QLabel *>(QLatin1String(name)); };
    QWidget *form = pane->findChild<QWidget *>(QStringLiteral("notebookPaneCellForm"));
    label("notebookPaneStatus")->clear();
    m_notebookPanePath.clear();
    m_notebookPaneCell = -1;
    Leaf *leaf = m_active;
    if (!leaf || leaf->view || !isNotebookFile(leaf->notePath)) {
        label("notebookPaneKernel")->setText(tr("Open a Jupyter notebook to see its kernel and cell metadata."));
        label("notebookPaneCell")->clear();
        form->hide();
        return;
    }
    const QString path = leaf->notePath;
    const QFileInfo info(path);
    const QString stamp = QStringLiteral("%1\n%2\n%3")
                              .arg(path, QString::number(info.lastModified().toMSecsSinceEpoch()),
                                   QString::number(info.size()));
    if (stamp != m_notebookDiskStamp) {
        // The file as a save will merge into: its cells are the ones the metadata belongs to.
        auto document = std::make_shared<notebook::IpynbDocument>();
        QByteArray bytes;
        if (!Vault::readBytes(path, &bytes) || !notebook::parseIpynb(bytes, document.get()))
            document.reset();
        m_notebookDisk = std::move(document);
        m_notebookDiskStamp = stamp;
    }
    const NotebookInfo notebookInfo = m_notebookInfo.value(path);
    label("notebookPaneKernel")
        ->setText(tr("Kernel: %1\nLanguage: %2")
                      .arg(notebookInfo.kernel.isEmpty() ? tr("none") : notebookInfo.kernel, notebookLanguageOf(path)));
    const QString text = leaf->editor->noteText();
    const QList<IpynbCell> cells = notebook::ipynbCells(text, notebookLanguageOf(path));
    const int shown = cellIndexAt(cells, leaf->editor->cursorOffset());
    const int fileCell =
        m_notebookDisk ? notebook::ipynbFileCellAt(*m_notebookDisk, text, leaf->editor->cursorOffset()) : -1;
    if (shown < 0 || fileCell < 0) {
        label("notebookPaneCell")
            ->setText(shown < 0 ? tr("No cells") : tr("Cell %1 is new: save the notebook to edit its metadata.").arg(shown + 1));
        form->hide();
        return;
    }
    const IpynbCell::Type type = cells.at(shown).type;
    label("notebookPaneCell")
        ->setText(tr("Cell %1 (%2)")
                      .arg(shown + 1)
                      .arg(type == IpynbCell::Type::Code       ? tr("code")
                           : type == IpynbCell::Type::Markdown ? tr("markdown")
                                                               : tr("raw")));
    m_notebookPanePath = path;
    m_notebookPaneCell = fileCell;
    const auto runs = m_notebookDirty.value(path);
    QJsonObject metadata = runs && runs->cellMetadata.contains(fileCell)
                               ? runs->cellMetadata.value(fileCell)
                               : notebook::ipynbCellMetadata(*m_notebookDisk, fileCell);
    QStringList tagList;
    for (const QJsonValue &tag : metadata.take(QStringLiteral("tags")).toArray())
        tagList << tag.toString();
    pane->findChild<QLineEdit *>(QStringLiteral("notebookPaneTags"))->setText(tagList.join(QStringLiteral(", ")));
    pane->findChild<QPlainTextEdit *>(QStringLiteral("notebookPaneMetadata"))
        ->setPlainText(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Indented)).trimmed());
    form->show();
}

void MainWindow::applyNotebookCellMetadata()
{
    QWidget *pane = m_notebookPane;
    if (!pane || m_notebookPaneCell < 0)
        return;
    auto *status = pane->findChild<QLabel *>(QStringLiteral("notebookPaneStatus"));
    const QString written = pane->findChild<QPlainTextEdit *>(QStringLiteral("notebookPaneMetadata"))->toPlainText();
    QJsonParseError parse{};
    const QJsonDocument document =
        QJsonDocument::fromJson(written.trimmed().isEmpty() ? QByteArray("{}") : written.toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        status->setText(parse.error != QJsonParseError::NoError
                            ? tr("Not applied: %1 at character %2").arg(parse.errorString()).arg(parse.offset)
                            : tr("Not applied: the metadata is a JSON object, {...}"));
        return;
    }
    QJsonObject metadata = document.object();
    QJsonArray tags;
    for (const QString &part : pane->findChild<QLineEdit *>(QStringLiteral("notebookPaneTags"))->text().split(QLatin1Char(','))) {
        const QString tag = part.trimmed();
        if (!tag.isEmpty() && !tags.contains(tag))
            tags.append(tag);
    }
    if (tags.isEmpty())
        metadata.remove(QStringLiteral("tags"));
    else
        metadata.insert(QStringLiteral("tags"), tags);
    std::shared_ptr<notebook::IpynbRuns> &runs = m_notebookDirty[m_notebookPanePath];
    if (!runs)
        runs = std::make_shared<notebook::IpynbRuns>();
    runs->cellMetadata.insert(m_notebookPaneCell, metadata);
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && leaf->notePath == m_notebookPanePath)
            leaf->editor->setModified(true);
    }
    status->setText(tr("Applied; written when the notebook is saved."));
    if (m_settings.boolean(QStringLiteral("files.autosave")))
        armAutosave();
}

} // namespace noteAppC
