#pragma once

#include <QWidget>

#include "PdfDocument.h"
#include "Theme.h"

class QAction;
class QLabel;
class QLineEdit;
class QSplitter;
class QToolBar;
class QTreeWidget;
class QUndoStack;

namespace noteAppC {

class FindBar;
class PdfPagesArea;

/// A PDF opened as a tab: the toolbar (page field, zoom, fit, the annotation
/// tools and their colours, an outline toggle), the outline panel, the page
/// canvas (PdfPagesArea, which does the real work) and the find bar, wired
/// together. This is MainWindow::openView's `.pdf` counterpart to CanvasView
/// - see the wiring snippets this lane sent back for the exact hookup.
class PdfView : public QWidget {
    Q_OBJECT

public:
    explicit PdfView(QWidget *parent = nullptr);

    bool load(const QString &path, QString *error = nullptr);
    bool save(QString *error = nullptr);
    bool isModified() const;
    QString path() const;
    QUndoStack *undoStack() const;

    void setTheme(const Theme &theme);

    /// Applies a link's subpath fragment (`#page=3`, Obsidian's own PDF
    /// grammar - see libs/render/PdfArgs.h, reused rather than reparsed) by
    /// jumping the view there. What MainWindow::openView passes the
    /// fragment of a `.pdf#page=N` link through to.
    void openFragment(const QString &subpath);

    /// pdf:extract-annotations. `notePath` empty uses
    /// defaultAnnotationsNotePath(path()) - the wiring snippet passes the
    /// vault's attachment-folder path instead when it has one configured.
    bool extractAnnotations(const QString &notePath = QString(), QString *error = nullptr);

signals:
    void modificationChanged(bool modified);
    /// A file link inside the PDF, resolved by MainWindow the same way any
    /// other relative path is (openRelative's own signature).
    void noteActivated(const QString &relativePath);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildToolbar();
    void buildOutlinePanel();
    void refreshPageField();
    void refreshOutline();
    void refreshToolButtons();
    void applyFindQuery();

    PdfPagesArea *m_pages = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLineEdit *m_pageField = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QSplitter *m_splitter = nullptr;
    QTreeWidget *m_outline = nullptr;
    FindBar *m_findBar = nullptr;

    QAction *m_toolSelect = nullptr;
    QAction *m_toolHighlight = nullptr;
    QAction *m_toolUnderline = nullptr;
    QAction *m_toolStrikeOut = nullptr;
    QAction *m_toolInk = nullptr;
    QAction *m_toolNote = nullptr;
    QAction *m_toolRectangle = nullptr;
    QAction *m_outlineToggle = nullptr;

    Theme m_theme;
};

} // namespace noteAppC
