#include "MarkdownEditor.h"

// input() from a running code block (docs/NOTEBOOKS.md): the kernel asks for a
// line, the block shows the prompt under its output with a line edit after it
// (getpass: masked), Return sends the line back and Esc interrupts the run.
// The editor knows nothing of the kernel: the runner hands it a prompt and a
// reply function through NotebookRunOptions::onInput, as it hands a result
// through the deliver callback. Nothing here runs unless a block asks.

#include <QKeyEvent>
#include <QLineEdit>
#include <QScrollBar>
#include <QTextLayout>

namespace noteAppC {

namespace {

/// Return and Esc on the line edit, eaten here: a QLineEdit lets Return go on to
/// its parent, which would type a newline into the note.
class PromptKeys : public QObject {
public:
    PromptKeys(QObject *parent, std::function<void(bool ok)> finish) : QObject(parent), m_finish(std::move(finish)) { }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
            return false;
        const int key = static_cast<QKeyEvent *>(event)->key();
        const bool isReturn = key == Qt::Key_Return || key == Qt::Key_Enter;
        if (!isReturn && key != Qt::Key_Escape)
            return false;
        if (event->type() == QEvent::KeyPress)
            m_finish(isReturn);
        return true;
    }

private:
    std::function<void(bool)> m_finish;
};

} // namespace

QLineEdit *MarkdownEditor::notebookInputEdit() const
{
    return qobject_cast<QLineEdit *>(m_notebookInput.edit.data());
}

MarkdownEditor::NotebookInputInfo MarkdownEditor::notebookInputInfo() const
{
    NotebookInputInfo info;
    info.active = bool(m_notebookInput.reply);
    info.prompt = m_notebookInput.prompt;
    info.password = m_notebookInput.password;
    if (m_notebookInput.edit) {
        info.visible = !m_notebookInput.edit->isHidden(); // not isVisible(): an editor that is not shown has no visible child
        info.editRect = m_notebookInput.edit->geometry();
    }
    return info;
}

void MarkdownEditor::showNotebookInput(const QString &key, quint64 generation, const NotebookInputPrompt &prompt,
                                       const NotebookInputReply &reply)
{
    // A run that is no longer this one's (the block was run again, cleared, or
    // finished while the question was on its way): nobody is waiting for it.
    const auto entry = m_notebookCache.constFind(key);
    if (entry == m_notebookCache.constEnd() || entry->generation != generation || !entry->running) {
        reply(false, QString());
        return;
    }
    finishNotebookInput(false); // one question at a time

    m_notebookInput.key = key;
    m_notebookInput.generation = generation;
    m_notebookInput.prompt = prompt.prompt;
    m_notebookInput.password = prompt.password;
    m_notebookInput.reply = reply;

    auto *edit = new QLineEdit(viewport());
    edit->setFrame(false);
    edit->setFont(fontFor({BlockSpan::Kind::Code, {0, 0}, 0}));
    edit->setEchoMode(prompt.password ? QLineEdit::Password : QLineEdit::Normal);
    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, m_theme.color(QStringLiteral("--background-modifier-hover")));
    palette.setColor(QPalette::Text, m_theme.color(QStringLiteral("--text-normal")));
    palette.setColor(QPalette::Highlight, m_theme.color(QStringLiteral("--interactive-accent")));
    edit->setPalette(palette);
    // Return answers; Esc interrupts the run instead. A filter on the edit alone (owned by it), none on the editor.
    edit->installEventFilter(new PromptKeys(edit, [this](bool ok) { finishNotebookInput(ok); }));
    edit->hide(); // positionNotebookInput shows it once its block is on screen
    m_notebookInput.edit = edit;

    // The block makes room for the row, then the edit sits in it.
    for (BlockView &block : m_blocks) {
        if (block.notebookKey != key)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
    if (!edit->isHidden())
        edit->setFocus();
}

void MarkdownEditor::finishNotebookInput(bool ok)
{
    if (!m_notebookInput.reply && !m_notebookInput.edit)
        return;
    // Cleared first: hiding the edit moves the focus, which must not finish it again.
    NotebookInputState done = std::move(m_notebookInput);
    m_notebookInput = NotebookInputState();
    QString line;
    if (done.edit) {
        if (const auto *edit = qobject_cast<QLineEdit *>(done.edit.data()))
            line = edit->text();
        done.edit->hide();
        done.edit->deleteLater();
        setFocus();
    }
    for (BlockView &block : m_blocks) {
        if (block.notebookKey != done.key)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
    if (done.reply)
        done.reply(ok, line); // after the row is gone: the answer may start the next ask at once
}

void MarkdownEditor::dropNotebookInput(const QString &key)
{
    if (!m_notebookInput.reply || m_notebookInput.key != key)
        return;
    m_notebookInput.reply = nullptr; // the run is over: there is nobody to answer
    if (m_notebookInput.edit) {
        m_notebookInput.edit->hide();
        m_notebookInput.edit->deleteLater();
        setFocus();
    }
    m_notebookInput = NotebookInputState();
}

void MarkdownEditor::appendNotebookInput(BlockView &view)
{
    if (!m_notebookInput.reply || view.notebookKey != m_notebookInput.key)
        return;
    // The prompt as the kernel wrote it (trailing space and all), then the edit's row.
    appendNotebookMessage(view, m_notebookInput.prompt.isEmpty() ? tr("Input:") : m_notebookInput.prompt,
                          QStringLiteral("--text-normal"));
    const qreal width = contentWidth() - 28;
    const qreal height = QFontMetricsF(fontFor({BlockSpan::Kind::Code, {0, 0}, 0})).height() + 8;
    view.notebookInputRect = QRectF(0, view.height + 4, width, height);
    view.height = view.notebookInputRect.bottom() + 8;
}

void MarkdownEditor::positionNotebookInput()
{
    QWidget *edit = m_notebookInput.edit;
    if (!edit)
        return;
    for (const BlockView &block : m_blocks) {
        if (block.notebookKey != m_notebookInput.key || !block.laidOut || block.notebookInputRect.isEmpty())
            continue;
        const QRectF rect = block.notebookInputRect.translated(contentLeft(), block.y - verticalScrollBar()->value());
        edit->setGeometry(rect.toRect());
        edit->setVisible(rect.bottom() > 0 && rect.top() < viewport()->height());
        return;
    }
    edit->hide(); // its block is not laid out (far off screen): back when it is
}

} // namespace noteAppC
