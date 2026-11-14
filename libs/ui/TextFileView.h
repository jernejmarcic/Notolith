// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Theme.h"
#include "Vault.h"

#include <QPlainTextEdit>
#include <QWidget>
#include <functional>

#include <memory>

namespace noteAppC {

class HtmlPageView;

/// A text file that is not a note - JSON, code, HTML, a log - shown and edited as plain text. An HTML file also
/// has a rendered page (HtmlPageView, litehtml: nothing runs, no network), shown whenever it is not in Source
/// mode. Bytes are read and written through Vault, so line endings and a BOM round-trip as they do for a note.
class TextFileView : public QWidget
{
    Q_OBJECT
public:
    explicit TextFileView(QWidget *parent = nullptr);
    ~TextFileView() override;

    /// Whether `path` is text this view takes. Reads the file capped like a note; `error` says why not (binary,
    /// too large, unreadable). On success `bytes` holds what was read.
    static bool probe(const QString &path, QByteArray *bytes, QString *error);

    bool load(const QString &path, QString *error);
    bool save(QString *error);
    bool isModified() const { return m_edit->document()->isModified(); }
    /// The source editor: what is typed, selected and saved, shown or not.
    QPlainTextEdit *editor() const { return m_edit; }
    QString toPlainText() const { return m_edit->toPlainText(); }
    bool isHtml() const;
    /// The rendered page is what shows (HTML only; false without litehtml). Switching to it draws the text now
    /// in the editor, saved or not.
    bool isRendered() const;
    void setRendered(bool rendered);
    HtmlPageView *page() const { return m_page; }
    /// Where a relative image or stylesheet is looked up: the file's own folder inside the vault, as `resolve`
    /// says, or nothing.
    void setLocalPathResolver(std::function<QString(const QString &)> resolve) { m_resolve = std::move(resolve); }
    const QString &path() const { return m_path; }
    void setPath(const QString &path);
    void setTheme(const Theme &theme);
    /// The file changed outside: reread it unless it has unsaved edits. True when the text on screen changed.
    bool reloadFromDisk();

signals:
    void modificationChanged(bool modified);
    void textChanged();
    /// A click on a link of the rendered page.
    void linkActivated(const QString &target);

private:
    void applyHighlighting();
    void showRendered();

    QPlainTextEdit *m_edit = nullptr;
    HtmlPageView *m_page = nullptr;
    std::function<QString(const QString &)> m_resolve;
    QString m_path;
    Note m_note; ///< line endings and BOM of the file, the text as last read or written
    Theme m_theme;
    struct Highlight;
    std::unique_ptr<Highlight> m_highlight;
};

} // namespace noteAppC
