#pragma once

#include <QScrollArea>

#include "ModuleHost.h"
#include "core/InkCanvas.h"

class QToolBar;

namespace noteAppC::ink {

/// The handwritten-note tab: a scroll area over the canvas and a small
/// toolbar (pen, eraser, undo, redo; Track S1 Task 16 grows it to every tool:
/// R14, PADD's pen has no eraser end and no working buttons). Load and
/// save go through InkFormat; a file whose version is above 1 opens
/// read-only, says so in the status bar, and is never written back (writeInk
/// stamps version 1, so saving it would downgrade the file).
class InkView : public ModuleView {
    Q_OBJECT
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
    void onEdited();

    const noteAppC::Settings *m_settings;
    QScrollArea *m_area;
    InkCanvas *m_canvas;
    QToolBar *m_toolBar;
    QAction *m_penAction;
    QAction *m_eraserAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QString m_path;
    bool m_readOnly = false;
    bool m_lastModified = false;
};

} // namespace noteAppC::ink
