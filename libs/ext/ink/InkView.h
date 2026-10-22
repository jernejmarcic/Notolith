#pragma once

#include <QFutureWatcher>
#include <QScrollArea>

#include "ModuleHost.h"
#include "core/InkCanvas.h"
#include "core/InkFormat.h"

class QLabel;
class QToolBar;

namespace noteAppC::ink {

/// The handwritten-note tab: a scroll area over the canvas and a small
/// toolbar (pen, eraser, undo, redo; Track S1 Task 16 grows it to every tool:
/// R14, PADD's pen has no eraser end and no working buttons). Load and
/// save go through InkFormat; a file whose version is above 1 opens
/// read-only, says so in the status bar and in the toolbar (the "readOnly"
/// property), and is never written back (writeInk stamps version 1, so saving
/// it would downgrade the file).
///
/// The vault is synced between machines, so the file can change under an open
/// tab. The view remembers the file's size and modification time at load and
/// after each own save; a save that finds them changed never overwrites the
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
    Q_PROPERTY(bool loading READ isLoading)
public:
    explicit InkView(const noteAppC::Settings *settings, QWidget *parent = nullptr);

    bool load(const QString &path, QString *error) override;
    bool save(QString *error) override;
    bool isModified() const override { return m_canvas->document().isModified(); }
    void setTheme(const Theme &theme) override;
    void applySettings(const noteAppC::Settings &settings) override;

    InkCanvas *canvas() const { return m_canvas; }
    QString path() const { return m_path; }
    bool isReadOnly() const { return m_readOnly; }
    bool isLoading() const { return m_loadWatcher != nullptr; }

private:
    struct DiskStamp {
        qint64 size = -1;
        qint64 modifiedMs = 0;
        bool operator==(const DiskStamp &) const = default;
    };
    static DiskStamp stampOf(const QString &path);
    static QString conflictPathFor(const QString &path);
    bool resolveExternalChange(QString *error);
    struct Parsed;
    void adopt(const QString &path, const DiskStamp &stamp, const InkReadResult &result);
    void onEdited();

    const noteAppC::Settings *m_settings;
    QScrollArea *m_area;
    InkCanvas *m_canvas;
    QToolBar *m_toolBar;
    QAction *m_penAction;
    QAction *m_eraserAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QAction *m_readOnlyAction;
    QAction *m_loadingAction;
    QAction *m_errorAction;   ///< the persistent "could not be read" notice of a tab whose first load failed
    QLabel *m_errorLabel;
    bool m_loadFailed = false; ///< no document was ever adopted: the tab stays read-only
    QFutureWatcher<Parsed> *m_loadWatcher = nullptr; ///< a file above kOffThreadParseBytes is parsing on the pool
    QString m_path;
    DiskStamp m_stamp; ///< the file as this view last read or wrote it
    bool m_readOnly = false;
    bool m_lastModified = false;
};

} // namespace noteAppC::ink
