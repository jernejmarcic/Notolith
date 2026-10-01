#include "MarkdownEditor.h"

// Completion and documentation from a running Jupyter kernel (docs/NOTEBOOKS.md):
// Ctrl+Space in a code block asks the block's kernel for completions at the
// caret and lists them; Shift+Tab asks for the documentation of what is under
// it. The editor never links libs/ext/notebook - the window supplies the
// NotebookIntrospector, as it does the NotebookRunner - and with none set, or
// no kernel already running for the note, none of this costs more than a
// pointer test on a key press.

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QListView>
#include <QPlainTextEdit>
#include <QScreen>
#include <QStringListModel>
#include <QTextDocument>
#include <QToolTip>

namespace noteAppC {

namespace {

/// A list longer than this is not read; the rest of the names are not worth the popup's height.
constexpr qsizetype kMaxMatches = 300;

bool isModifierKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
           || key == Qt::Key_AltGr || key == Qt::Key_CapsLock;
}

} // namespace

void MarkdownEditor::setNotebookIntrospector(NotebookIntrospector introspector)
{
    m_notebookIntrospector = std::move(introspector);
    hideNotebookIntrospection();
}

QStringList MarkdownEditor::notebookCompletionRows() const
{
    QStringList rows;
    if (!m_notebookCompleter || !m_notebookCompleter->popup()->isVisible())
        return rows;
    const QAbstractItemModel *model = m_notebookCompleter->popup()->model();
    for (int row = 0; model && row < model->rowCount(); ++row)
        rows << model->index(row, 0).data().toString();
    return rows;
}

QString MarkdownEditor::notebookInspectText() const
{
    const auto *view = qobject_cast<const QPlainTextEdit *>(m_notebookInspect);
    return view && view->isVisible() ? view->toPlainText() : QString();
}

void MarkdownEditor::hideNotebookIntrospection()
{
    ++m_introGeneration; // an answer still on its way is not wanted any more
    if (m_notebookCompleter)
        m_notebookCompleter->popup()->hide();
    if (m_notebookInspect)
        m_notebookInspect->hide();
}

bool MarkdownEditor::handleNotebookIntrospectKey(QKeyEvent *event)
{
    const int key = event->key();
    if (isModifierKey(key))
        return false;
    const bool completing = m_notebookCompleter && m_notebookCompleter->popup()->isVisible();
    const bool documenting = m_notebookInspect && m_notebookInspect->isVisible();
    if (completing) {
        QAbstractItemView *popup = m_notebookCompleter->popup();
        const QAbstractItemModel *rows = popup->model();
        switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab: {
            const QString match = popup->currentIndex().data().toString();
            hideNotebookIntrospection();
            if (!match.isEmpty())
                acceptNotebookCompletion(match);
            return true;
        }
        case Qt::Key_Escape:
            hideNotebookIntrospection();
            return true;
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const int count = rows ? rows->rowCount() : 0;
            if (count > 0) {
                const int step = key == Qt::Key_Down ? 1 : -1;
                popup->setCurrentIndex(rows->index((qMax(0, popup->currentIndex().row()) + step + count) % count, 0));
            }
            return true;
        }
        default: // typing goes on in the note: the list is for the caret it was asked at
            hideNotebookIntrospection();
            break;
        }
    } else if (documenting) {
        const bool escape = key == Qt::Key_Escape;
        hideNotebookIntrospection();
        if (escape)
            return true;
    }

    const bool completionKey = key == Qt::Key_Space && (event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier;
    const bool inspectKey = key == Qt::Key_Backtab || (key == Qt::Key_Tab && event->modifiers().testFlag(Qt::ShiftModifier));
    if ((!completionKey && !inspectKey) || m_mode == Mode::Reading)
        return false;
    return requestNotebookIntrospection(inspectKey, completionKey);
}

bool MarkdownEditor::requestNotebookIntrospection(bool inspect, bool tellWhenNone)
{
    if (!m_features.notebooks || !m_notebookIntrospector)
        return false;
    const CodeFenceSpan *fence = nullptr;
    for (const CodeFenceSpan &candidate : m_decorations.codeFences) {
        if (candidate.depth == 0 && candidate.content.start <= m_cursor && m_cursor <= candidate.content.end
            && !candidate.language.trimmed().isEmpty()) {
            fence = &candidate;
            break;
        }
    }
    if (!fence)
        return false;

    NotebookRunOptions options;
    for (const CodeOptions &declared : m_decorations.codeOptions) {
        if (declared.block.overlaps(fence->content)) {
            options.sandboxHost = declared.sandboxHost;
            options.kernel = declared.kernel;
            break;
        }
    }
    const qsizetype origin = fence->content.start;
    const QString code = m_text.mid(origin, fence->content.end - origin);

    hideNotebookIntrospection(); // also makes this request the only one that counts
    const quint64 generation = m_introGeneration;
    m_introVersion = m_textVersion;
    m_introCursor = m_cursor;
    const bool sent = m_notebookIntrospector(
        fence->language.trimmed().toLower(), code, m_cursor - origin, inspect, options,
        [this, lifeline = m_lifeline, generation, inspect, origin](NotebookIntrospection answer) {
            lifeline->post(this, [this, generation, inspect, origin, answer = std::move(answer)] {
                if (generation != m_introGeneration || m_textVersion != m_introVersion || m_cursor != m_introCursor)
                    return; // typed or moved since: it answers a question nobody is asking
                if (inspect) {
                    if (answer.text.trimmed().isEmpty())
                        QToolTip::showText(viewport()->mapToGlobal(cursorPoint().toPoint()), tr("No documentation found"), this);
                    else
                        showNotebookInspect(answer.text);
                } else if (answer.matches.isEmpty()) {
                    QToolTip::showText(viewport()->mapToGlobal(cursorPoint().toPoint()), tr("No completions"), this);
                } else {
                    showNotebookCompletion(answer.matches, origin + answer.from, origin + answer.to);
                }
            });
        });
    if (!sent && tellWhenNone) {
        QToolTip::showText(viewport()->mapToGlobal(cursorPoint().toPoint()),
                           tr("No kernel is running for this note's %1 blocks: run one with the kernel on first")
                               .arg(fence->language.trimmed()),
                           this);
    }
    return sent || tellWhenNone;
}

void MarkdownEditor::showNotebookCompletion(const QStringList &all, qsizetype from, qsizetype to)
{
    const QStringList matches = all.size() > kMaxMatches ? all.mid(0, kMaxMatches) : all;
    from = qBound<qsizetype>(0, from, m_text.size());
    to = qBound<qsizetype>(from, to, m_text.size());
    m_introFrom = from;
    m_introTo = to;
    if (matches.size() == 1) { // as Jupyter does: one way to finish it is not a choice
        acceptNotebookCompletion(matches.first());
        return;
    }
    if (!m_notebookCompleter) {
        m_notebookCompleter = new QCompleter(new QStringListModel(this), this);
        m_notebookCompleter->setWidget(this);
        m_notebookCompleter->popup()->setObjectName(QStringLiteral("notebookCompletePopup"));
        if (auto *list = qobject_cast<QListView *>(m_notebookCompleter->popup()))
            list->setUniformItemSizes(true);
        m_notebookCompleter->setCompletionMode(QCompleter::PopupCompletion);
        m_notebookCompleter->setFilterMode(Qt::MatchContains);
        m_notebookCompleter->setMaxVisibleItems(10);
        // The kernel has already filtered and ranked: every row stays.
        connect(m_notebookCompleter, qOverload<const QString &>(&QCompleter::activated), this,
                [this](const QString &match) {
                    hideNotebookIntrospection();
                    acceptNotebookCompletion(match);
                });
    }
    qobject_cast<QStringListModel *>(m_notebookCompleter->model())->setStringList(matches);
    m_notebookCompleter->setCompletionPrefix(QString());
    m_notebookCompleter->popup()->setCurrentIndex(m_notebookCompleter->completionModel()->index(0, 0));
    QRect anchor(cursorPoint().toPoint(), QSize(1, int(QFontMetricsF(font()).height())));
    anchor.moveTopLeft(viewport()->mapTo(this, anchor.topLeft()));
    anchor.setWidth(m_notebookCompleter->popup()->sizeHintForColumn(0) + 32);
    m_notebookCompleter->complete(anchor);
}

void MarkdownEditor::acceptNotebookCompletion(const QString &match)
{
    const qsizetype from = qBound<qsizetype>(0, m_introFrom, m_text.size());
    const qsizetype to = qBound<qsizetype>(from, m_introTo, m_text.size());
    applyEdit(from, to - from, match);
}

void MarkdownEditor::showNotebookInspect(const QString &text)
{
    if (!m_notebookInspect) {
        auto *view = new QPlainTextEdit(this);
        view->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        view->setObjectName(QStringLiteral("notebookInspect"));
        view->setReadOnly(true);
        view->setFocusPolicy(Qt::NoFocus);
        view->setAttribute(Qt::WA_ShowWithoutActivating);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_notebookInspect = view;
    }
    auto *view = static_cast<QPlainTextEdit *>(m_notebookInspect);
    // Like the completion list and the other popups: the theme's own colours.
    view->setStyleSheet(QStringLiteral("QPlainTextEdit#notebookInspect { background: %1; color: %2; "
                                       "border: 1px solid %3; border-radius: 6px; padding: 6px; }")
                            .arg(m_theme.color(QStringLiteral("--background-secondary")).name(),
                                 m_theme.color(QStringLiteral("--text-normal")).name(),
                                 m_theme.color(QStringLiteral("--background-modifier-border")).name()));
    view->setPlainText(text.trimmed());
    const int lineHeight = int(QFontMetricsF(font()).height());
    const QFontMetrics metrics(view->font());
    int longest = 0;
    for (const QString &line : text.split(QLatin1Char('\n')))
        longest = qMax(longest, int(line.size()));
    const int width = qBound(260, metrics.horizontalAdvance(QLatin1Char('0')) * longest + 40, 680);
    view->document()->setTextWidth(width - 40);
    const int height = qBound(40, int(view->document()->size().height()) + 30, 320);
    view->resize(width, height);

    QPoint at = viewport()->mapToGlobal(cursorPoint().toPoint() + QPoint(0, lineHeight));
    if (const QScreen *screen = QGuiApplication::screenAt(at)) {
        const QRect room = screen->availableGeometry();
        if (at.y() + height > room.bottom())
            at.setY(at.y() - height - lineHeight); // above the line instead
        at.setX(qMax(room.left(), qMin(at.x(), room.right() - width)));
    }
    view->move(at);
    view->show();
}

} // namespace noteAppC
