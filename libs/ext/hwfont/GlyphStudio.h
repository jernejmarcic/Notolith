#pragma once

#include <map>
#include <vector>

#include <QColor>
#include <QString>

#include "GlyphSet.h"
#include "GlyphStore.h"
#include "ModuleHost.h"
#include "InkCanvas.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace noteAppC::hwfont {

/// The glyph studio (spec section 9): write a letter, save it, move on to the next. A file-less tab
/// (R6): not opened from a path, it saves its cell through save() when the host closes it or quits.
/// Every action is on a key and on a button (R14: PADD's pen is tip-only); the pen's barrel click
/// saves too, detected on an event filter over the canvas so inkcore needs no signal for it.
class GlyphStudio : public ModuleView {
    Q_OBJECT
public:
    GlyphStudio(ModuleHost &host, const QString &fontName, QWidget *parent = nullptr);

    bool load(const QString &, QString *) override { return true; } ///< not file-backed: opened by the command
    /// R6: writes the current cell as saveCurrentAndAdvance does, without moving on; an empty cell is nothing to write.
    bool save(QString *error) override;
    /// R6: the cell holds strokes that are neither saved nor discarded.
    bool isModified() const override { return !m_canvas->document().items().empty(); }
    void setTheme(const Theme &theme) override;

    char32_t currentTarget() const { return m_target; }
    void saveCurrentAndAdvance(); ///< Enter, the Save button, the pen's barrel click
    void discardCurrent();        ///< Backspace, the Discard button: clears the cell without saving
    void retarget(char32_t codepoint); ///< a click in the glyph list: the cell is kept
    void skipToNext();            ///< PageDown, the Skip button: on to the next glyph that needs samples
    ink::InkCanvas *canvas() const { return m_canvas; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class Guides;
    bool commitCell(QString *error);
    void clearCell();
    void refreshList();
    void showTarget();
    void showError(const QString &text);
    void onEdited();
    char32_t nextNeedingMore(char32_t after) const;
    bool needsMore(char32_t codepoint) const;
    bool handleBarrel(QTabletEvent *event);

    GlyphStore m_store;
    GlyphMetrics m_metrics;
    std::vector<GlyphTarget> m_set;
    std::map<char32_t, int> m_have; ///< samples on disk per glyph, as of the last refreshList()
    char32_t m_target = 0;
    ink::InkCanvas *m_canvas;
    Guides *m_guides;
    QLabel *m_glyphLabel;
    QLabel *m_countLabel;
    QLabel *m_errorLabel;
    QListWidget *m_list;
    QColor m_doneColor;
    bool m_lastModified = false;
    bool m_barrelDown = false;   ///< a press with the barrel button held, until its release
    bool m_barrelMoved = false;
    QPointF m_barrelStart;
};

} // namespace noteAppC::hwfont
