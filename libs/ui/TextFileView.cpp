// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextFileView.h"

#include <QFileInfo>
#include <QPalette>
#include <QScrollBar>

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
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("textFileView"));
    setFrameShape(QFrame::NoFrame);
}

TextFileView::~TextFileView() = default;

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
    setLineWrapMode(code ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
    setPlainText(m_note.text);
    document()->setModified(false);
    applyHighlighting();
    return true;
}

bool TextFileView::save(QString *error)
{
    Note note = m_note;
    note.text = toPlainText();
    if (!Vault::write(m_path, note, error))
        return false;
    m_note = note;
    document()->setModified(false);
    return true;
}

void TextFileView::setPath(const QString &path)
{
    m_path = path;
    applyHighlighting();
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
    const int scroll = verticalScrollBar()->value();
    setPlainText(disk.text);
    document()->setModified(false);
    verticalScrollBar()->setValue(scroll);
    return true;
}

void TextFileView::setTheme(const Theme &theme)
{
    m_theme = theme;
    setFont(theme.monospaceFont());
    QPalette palette = this->palette();
    palette.setColor(QPalette::Base, theme.color(QStringLiteral("--background-primary")));
    palette.setColor(QPalette::Text, theme.color(QStringLiteral("--text-normal")));
    setPalette(palette);
    applyHighlighting();
}

void TextFileView::applyHighlighting()
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    if (m_path.isEmpty())
        return;
    if (!m_highlight)
        m_highlight = std::make_unique<Highlight>(document());
    using Repository = KSyntaxHighlighting::Repository;
    m_highlight->highlighter.setTheme(m_highlight->repository.defaultTheme(
        m_theme.isDark() ? Repository::DarkTheme : Repository::LightTheme));
    m_highlight->highlighter.setDefinition(m_highlight->repository.definitionForFileName(m_path));
#endif
}

} // namespace noteAppC
