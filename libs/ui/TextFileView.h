// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Theme.h"
#include "Vault.h"

#include <QPlainTextEdit>

#include <memory>

namespace noteAppC {

/// A text file that is not a note - JSON, code, HTML, a log - shown and edited as plain text. Nothing here renders
/// or runs the content: HTML is source. Bytes are read and written through Vault, so line endings and a BOM
/// round-trip as they do for a note.
class TextFileView : public QPlainTextEdit
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
    bool isModified() const { return document()->isModified(); }
    const QString &path() const { return m_path; }
    void setPath(const QString &path);
    void setTheme(const Theme &theme);
    /// The file changed outside: reread it unless it has unsaved edits. True when the text on screen changed.
    bool reloadFromDisk();

private:
    void applyHighlighting();

    QString m_path;
    Note m_note; ///< line endings and BOM of the file, the text as last read or written
    Theme m_theme;
    struct Highlight;
    std::unique_ptr<Highlight> m_highlight;
};

} // namespace noteAppC
