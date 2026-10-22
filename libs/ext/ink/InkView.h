#pragma once

#include <functional>
#include <optional>

#include <QList>
#include <QPair>
#include <QVariant>

#include <QFutureWatcher>
#include <QScrollArea>

#include "ModuleHost.h"
#include "Theme.h"
#include "core/InkCanvas.h"
#include "core/InkFormat.h"

class QActionGroup;
class QButtonGroup;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QShowEvent;
class QTimer;
class QToolBar;
class QToolButton;

namespace noteAppC::ink {

/// The handwritten-note tab: a scroll area over the canvas and a toolbar: the five tools (pen, eraser with its
/// whole/parts toggle, select with a lasso or a rectangle, shape with its four kinds, text), Draw with mouse, the pen
/// (width, six colours, pressure curve), undo and redo, the zoom and the Page menu (format, layout, pattern, page
/// colour, add a page). R14: every tool is on the toolbar and on its key, because PADD's pen has no eraser end and no
/// working buttons. The eraser mode, the select shape and the shape kind are remembered in the settings (ink.*), and so
/// is what a pen's pressure range was learned to be. A notice (read-only, loading, could not be read) is a banner above
/// the canvas, never in the toolbar, where a narrow window would hide it in the overflow menu. Load and
/// save go through InkFormat; a file whose version is above 1 opens
/// read-only, says so in the status bar and in the toolbar (the "readOnly"
/// property), and is never written back (writeInk stamps version 1, so saving
/// it would downgrade the file).
///
/// The vault is synced between machines, so the file can change under an open
/// tab. The view remembers the file's size and modification time at load and
/// after each own save. The host's file watcher calls reloadFromDisk() when the
/// file changes: an unmodified view reloads at once, a modified one keeps its
/// edits and says so. A save that finds them changed never overwrites the
/// file: an unmodified view reloads, a modified one writes its document to a
/// "<name> (conflict <date time>).ink" beside the file, reloads the disk
/// version and says so.
///
/// A file above kOffThreadParseBytes is read on the UI thread (cheap) and parsed on the pool: load()
/// returns true at once, the toolbar says "Loading...", the canvas ignores the pen and save() refuses
/// ("still loading") until the parsed document is swapped in.
class InkView : public ModuleView {
    Q_OBJECT
    Q_PROPERTY(bool readOnly READ isReadOnly)
public:
    /// `settings` is the host's: the view reads the ink.* keys and writes the few it owns (the choices above, ink.drawWithMouse,
    /// ink.pressureByDevice). Null (a bare test) keeps every default and writes nothing.
    explicit InkView(noteAppC::Settings *settings, QWidget *parent = nullptr);
    ~InkView() override;
    /// The choices of the layout and pattern rows, shared with the module's Settings rows: (label, name as stored).
    static QList<QPair<QString, QVariant>> layoutChoices();
    static QList<QPair<QString, QVariant>> patternChoices();

    bool load(const QString &path, QString *error) override;
    bool save(QString *error) override;
    bool isModified() const override { return m_canvas->document().isModified() || m_canvas->isTextEditDirty(); } ///< typed, not yet committed text counts: closing the tab must offer to save it
    void setTheme(const Theme &theme) override;
    void applySettings(const noteAppC::Settings &settings) override;
    /// The learned pressures are written a moment after the last raise (kPressureSaveMs), once for a run of them; this
    /// writes them now. The window calls it before its Settings go, the view's own destructor after a closed tab.
    void flushSettings() override;
    static constexpr int kPressureSaveMs = 1500;

    InkCanvas *canvas() const { return m_canvas; }
    QString path() const { return m_path; }
    bool isReadOnly() const { return m_readOnly; }
    bool isLoading() const override { return m_loadWatcher != nullptr; }
    void reloadFromDisk() override;
    void setFilePath(const QString &path) override;

protected:
    void showEvent(QShowEvent *event) override;

private:
    struct DiskStamp {
        qint64 size = -1;
        qint64 modifiedMs = 0;
        bool operator==(const DiskStamp &) const = default;
    };
    static DiskStamp stampOf(const QString &path);
    static QString conflictPathFor(const QString &path, const QString &word = QStringLiteral("conflict"));
    bool resolveExternalChange(QString *error);
    struct Parsed;
    void adopt(const QString &path, const DiskStamp &stamp, const InkReadResult &result);
    void onEdited();
    void buildTools(QActionGroup *tools);
    void buildToolBar();
    void buildPageMenu();
    void updateEditActions(); ///< Delete and Copy follow the selection, Paste the clipboard
    void setToolsEnabled(bool on); ///< a read-only or unreadable document: nothing on the toolbar edits it
    void remember(const char *key, const QString &value); ///< writes a setting this view owns, and saves it
    void applyStoredChoices(const noteAppC::Settings &settings);
    void learnedPressure(const QString &device, double max);
    void selectPenColour(int index);
    void refreshSwatches();   ///< the pen colours follow the page: light ink on a dark page
    void showZoom(double zoom);
    void showShapeKind(ShapeKind kind);
    /// One page change: `edit` alters a copy of the settings, which the document takes (or refuses: nothing changes and the
    /// view says so), as one history step. A page format or a layout change refits the extent to whole pages.
    void changePage(const std::function<void(DocumentSettings &)> &edit);
    void addPage();
    void syncPageMenu();      ///< the Page menu shows the document's settings (after a load, an undo, a change)
    QPixmap renderText(const QString &markdown, const QSizeF &size, qreal scale) const;
    QWidget *makeTextEditor(const QString &markdown) const;
    Theme paperTheme(bool clear) const; ///< the page's theme for the text on it: see renderText

    noteAppC::Settings *m_settings;
    QScrollArea *m_area;
    InkCanvas *m_canvas;
    QToolBar *m_toolBar;
    QAction *m_penAction;      ///< inkTool_pen
    QAction *m_eraserAction;   ///< inkTool_eraser
    QAction *m_splitAction;    ///< checkable: erase parts of strokes (EraserMode::Split)
    QAction *m_selectAction;       ///< inkTool_lasso, the select tool (Tool::Lasso); its menu picks the shape
    QAction *m_selectLassoAction;  ///< checkable, exclusive with the rectangle (SelectShape::Lasso)
    QAction *m_selectRectAction;   ///< checkable: select with a rectangle instead of a lasso (SelectShape::Rect)
    QAction *m_shapeAction;        ///< inkTool_shape (Tool::Shape); its menu holds the four kinds and its face shows the current one
    QAction *m_shapeKindActions[4]; ///< line, arrow, rectangle, ellipse: checkable, exclusive
    QAction *m_textAction;         ///< inkTool_text (Tool::Text)
    QAction *m_mouseAction;        ///< checkable: Draw with mouse (ink.drawWithMouse)
    QAction *m_deleteAction;
    QAction *m_copyAction;
    QAction *m_pasteAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QAction *m_addPageAction;
    QLabel *m_readOnlyLabel;  ///< the three notices are labels above the canvas
    QLabel *m_loadingLabel;
    QLabel *m_errorLabel;     ///< the persistent "could not be read" notice of a tab whose first load failed
    QDoubleSpinBox *m_penWidth = nullptr;
    QList<QToolButton *> m_penSwatches;
    int m_penColourIndex = 0;
    QColor m_swatchPage;      ///< the page colour the swatches were last made for
    QComboBox *m_curveCombo = nullptr;
    QComboBox *m_zoomCombo = nullptr;
    QToolButton *m_pageButton = nullptr;
    QComboBox *m_pageFormat = nullptr;
    QDoubleSpinBox *m_pageWidth = nullptr;
    QDoubleSpinBox *m_pageHeight = nullptr;
    QComboBox *m_pageLayout = nullptr;
    QComboBox *m_pagePattern = nullptr;
    QDoubleSpinBox *m_patternSize = nullptr;
    QList<QToolButton *> m_pageSwatches;
    QList<QWidget *> m_editWidgets; ///< disabled with the tools when the document cannot be edited
    std::optional<Theme> m_hostTheme; ///< the app's theme: only its font overrides reach the text boxes (renderText)
    bool m_loadFailed = false; ///< no document was ever adopted: the tab stays read-only
    QFutureWatcher<Parsed> *m_loadWatcher = nullptr; ///< a file above kOffThreadParseBytes is parsing on the pool
    QString m_path;
    DiskStamp m_stamp; ///< the file as this view last read or wrote it
bool m_diskUnreadable = false; ///< the last look at the file failed to parse it: save() keeps its bytes before writing over them
    QTimer *m_pressureSave = nullptr; ///< single shot: the learned pressures reach the disk once for a run of raises
    bool m_toolsOn = true;
    bool m_readOnly = false;
    bool m_lastModified = false;
};

} // namespace noteAppC::ink
