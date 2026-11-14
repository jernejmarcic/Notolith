// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextFileView.h"

#include "HtmlPageView.h"

#include <QFileInfo>
#include <QPalette>
#include <QScrollBar>
#include <QStackedLayout>

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

namespace noteAppC {

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
struct TextFileView::Highlight {
    KSyntaxHighlighting::Repository repository;
    KSyntaxHighlighting::SyntaxHighlighter highlighter;
    explicit Highlight(QTextDocument *document)
        : highlighter(document)
    {
    }
};
#else
struct TextFileView::Highlight {};
#endif

TextFileView::TextFileView(QWidget *parent)
    : QWidget(parent)
    , m_edit(new QPlainTextEdit(this))
{
    setObjectName(QStringLiteral("textFileView"));
    m_edit->setFrameShape(QFrame::NoFrame);
    auto *stack = new QStackedLayout(this);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->addWidget(m_edit);
    setFocusProxy(m_edit);
    connect(m_edit, &QPlainTextEdit::modificationChanged, this, &TextFileView::modificationChanged);
    connect(m_edit, &QPlainTextEdit::textChanged, this, &TextFileView::textChanged);
}

bool TextFileView::isHtml() const
{
    return m_path.endsWith(QLatin1String(".html"), Qt::CaseInsensitive)
           || m_path.endsWith(QLatin1String(".htm"), Qt::CaseInsensitive);
}

bool TextFileView::isRendered() const
{
    return m_page && static_cast<QStackedLayout *>(layout())->currentWidget() == m_page;
}

void TextFileView::setRendered(bool rendered)
{
    auto *stack = static_cast<QStackedLayout *>(layout());
    if (rendered && isHtml() && HtmlRenderer::isAvailable()) {
        if (!m_page) {
            m_page = new HtmlPageView(this);
            m_page->setTheme(m_theme);
            connect(m_page, &HtmlPageView::linkActivated, this, &TextFileView::linkActivated);
            stack->addWidget(m_page);
        }
        showRendered();
        stack->setCurrentWidget(m_page);
        setFocusProxy(m_page);
    } else {
        stack->setCurrentWidget(m_edit);
        setFocusProxy(m_edit);
    }
}

void TextFileView::showRendered()
{
    m_page->setContent(m_edit->toPlainText(), QFileInfo(m_path).absolutePath(), m_resolve);
}

TextFileView::~TextFileView()
{
    // The highlighter's own teardown re-edits the document: nobody is told, the window may be going too.
    m_edit->blockSignals(true);
    m_edit->document()->blockSignals(true);
}

bool TextFileView::probe(const QString &path, QByteArray *bytes, QString *error)
{
    if (!Vault::readBytes(path, bytes, error))
        return false;
    if (bytes->left(8192).contains('\0') || Vault::decode(*bytes).lossy) {
        if (error)
            *error = tr("%1 is a binary file").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

bool TextFileView::load(const QString &path, QString *error)
{
    QByteArray bytes;
    if (!probe(path, &bytes, error))
        return false;
    m_path = path;
    m_note = Vault::decode(bytes);
    // The file name, not the text, picks the language: a suffix, else a name like Makefile.
    const bool code = !(path.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)
                        || path.endsWith(QLatin1String(".log"), Qt::CaseInsensitive)
                        || QFileInfo(path).suffix().isEmpty());
    m_edit->setLineWrapMode(code ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
    m_edit->setPlainText(m_note.text);
    m_edit->document()->setModified(false);
    applyHighlighting();
    if (isRendered())
        showRendered();
    return true;
}

bool TextFileView::save(QString *error)
{
    Note note = m_note;
    note.text = m_edit->toPlainText();
    if (!Vault::write(m_path, note, error))
        return false;
    m_note = note;
    m_edit->document()->setModified(false);
    return true;
}

void TextFileView::setPath(const QString &path)
{
    m_path = path;
    applyHighlighting();
    if (isRendered())
        showRendered(); // the folder it looks images up in moved too
}

bool TextFileView::reloadFromDisk()
{
    QByteArray bytes;
    if (isModified() || !probe(m_path, &bytes, nullptr))
        return false;
    const Note disk = Vault::decode(bytes);
    if (disk.text == m_note.text && disk.crlf == m_note.crlf && disk.bom == m_note.bom && disk.mixed == m_note.mixed)
        return false; // this window's own save
    m_note = disk;
    const int scroll = m_edit->verticalScrollBar()->value();
    m_edit->setPlainText(disk.text);
    m_edit->document()->setModified(false);
    m_edit->verticalScrollBar()->setValue(scroll);
    if (isRendered())
        showRendered();
    return true;
}

void TextFileView::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_edit->setFont(theme.monospaceFont());
    QPalette palette = m_edit->palette();
    palette.setColor(QPalette::Base, theme.color(QStringLiteral("--background-primary")));
    palette.setColor(QPalette::Text, theme.color(QStringLiteral("--text-normal")));
    m_edit->setPalette(palette);
    if (m_page)
        m_page->setTheme(theme);
    applyHighlighting();
}

void TextFileView::applyHighlighting()
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    if (m_path.isEmpty())
        return;
    if (!m_highlight)
        m_highlight = std::make_unique<Highlight>(m_edit->document());
    using Repository = KSyntaxHighlighting::Repository;
    m_highlight->highlighter.setTheme(m_highlight->repository.defaultTheme(
        m_theme.isDark() ? Repository::DarkTheme : Repository::LightTheme));
    m_highlight->highlighter.setDefinition(m_highlight->repository.definitionForFileName(m_path));
#endif
}

} // namespace noteAppC
