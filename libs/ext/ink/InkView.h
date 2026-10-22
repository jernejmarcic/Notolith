#pragma once

#include <QScrollArea>

#include "ModuleHost.h"
#include "core/InkCanvas.h"

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
class InkView : public ModuleView {
    Q_OBJECT
    Q_PROPERTY(bool readOnly READ isReadOnly)
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

private:
    struct DiskStamp {
        qint64 size = -1;
        qint64 modifiedMs = 0;
        bool operator==(const DiskStamp &) const = default;
    };
    static DiskStamp stampOf(const QString &path);
    static QString conflictPathFor(const QString &path);
    bool resolveExternalChange(QString *error);
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
    QString m_path;
    DiskStamp m_stamp; ///< the file as this view last read or wrote it
    bool m_readOnly = false;
    bool m_lastModified = false;
};

} // namespace noteAppC::ink
