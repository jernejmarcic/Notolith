#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "InkTypes.h"
#include "SpatialGrid.h"

namespace noteAppC::ink {

/// The document: settings plus items in creation order, a spatial grid over
/// their bounds, and undo/redo as snapshots (spec section 7). Edits mutate the
/// live state; commit() closes an edit (pen-up, an erase, a lasso commit) and
/// is what undo returns to. Snapshots share the stroke pointers.
class InkDocument {
public:
    static constexpr int kHistoryCap = 100;
    static constexpr double kPageGap = 16.0; ///< ContinuousVertical: widget-space gap between pages (scaled by the zoom, never part of document coordinates)
    static constexpr int kMaxPages = 100000; ///< growToContain refuses past this; pageCount() clamps to it

    InkDocument();

    const DocumentSettings &settings() const { return m_settings; }
    /// False, and nothing changes, for sizes the reader would refuse (DocumentSettings::isUsable): a file this
    /// writes must read back. The settings UI (Track S1 Task 16) reports the refusal.
    bool setSettings(const DocumentSettings &settings);
    const QJsonObject &extraTopLevel() const { return m_extraTop; }
    void setExtraTopLevel(const QJsonObject &extra)
    {
        m_extraTop = extra;
        touch();
    }

    const std::vector<Item> &items() const { return m_items; }
    qint64 addStroke(std::shared_ptr<const Stroke> stroke, Layer layer = Layer::User);
    qint64 addItem(Item item);
    bool removeItem(qint64 t);
    bool replaceItem(qint64 t, Item item);
    const Item *item(qint64 t) const;
    std::vector<const Item *> paintOrder(const QRectF &area) const;
    std::vector<qint64> itemsIntersecting(const QRectF &area) const;
    QRectF contentBounds() const;

    void commit();
    bool undo();
    bool redo();
    /// Drops edits no commit has closed: back to the last committed snapshot, the undo and redo stacks
    /// untouched. A no-op (false) on a clean document, unlike undo(), which then steps back in history.
    bool discardLiveEdits();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    quint64 revision() const { return m_revision; }
    void markSaved() { m_savedRevision = m_revision; }
    bool isModified() const { return m_revision != m_savedRevision; }

    int pageCount() const;
    QRectF pageRect(int index) const;
    bool growToContain(const QRectF &bounds);
    void addPage();

    /// A load: replaces everything, clears the history, counts as saved.
    void reset(DocumentSettings settings, std::vector<Item> items, QJsonObject extraTop);

private:
    struct Snapshot {
        std::vector<Item> items;
        DocumentSettings settings;
        qint64 nextT = 1;
        quint64 revision = 0;
    };
    Snapshot snapshot() const { return {m_items, m_settings, m_nextT, m_revision}; }
    void restore(const Snapshot &snapshot);
    void reindex();
    std::vector<Item>::const_iterator find(qint64 t) const;
    std::vector<Item>::iterator find(qint64 t);
    void touch() { ++m_revision; }

    DocumentSettings m_settings;
    QJsonObject m_extraTop;
    std::vector<Item> m_items; ///< ascending t
    SpatialGrid m_grid;
    qint64 m_nextT = 1;
    quint64 m_revision = 0;
    quint64 m_savedRevision = 0;
    Snapshot m_committed;
    std::deque<Snapshot> m_undo;
    std::deque<Snapshot> m_redo;
};

} // namespace noteAppC::ink
