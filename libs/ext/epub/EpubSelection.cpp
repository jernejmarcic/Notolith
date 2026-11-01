#include "EpubView.h"

#include "EpubPosition.h"
#include "FindBar.h"
#include "Vault.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>

#include <limits>

/// Hit testing (pixel -> chapter + character offset via HtmlRenderer's
/// text-run map), selection, highlight CRUD and its sidecar persistence, the
/// table of contents tree, find-in-book, and the remembered reading
/// position.

namespace noteAppC {

QString EpubView::chapterPlainText(const RenderedChapter &chapter)
{
    QString text;
    qsizetype expected = -1;
    for (const HtmlRenderer::TextRun &run : chapter.textRuns) {
        if (expected >= 0 && run.offset > expected)
            text += QLatin1Char(' ');
        text += run.text;
        expected = run.offset + run.text.size();
    }
    return text;
}

qint64 EpubView::topForOffset(const RenderedChapter &chapter, qsizetype offset)
{
    for (const HtmlRenderer::TextRun &run : chapter.textRuns) {
        if (run.offset + run.text.size() > offset)
            return qint64(run.rect.top());
    }
    return 0;
}

EpubView::HitPoint EpubView::hitTest(const QPoint &contentPos)
{
    HitPoint result;
    if (!m_document.isValid())
        return result;

    // Continuous and single-column paged both start their one page at
    // document-Y == m_vscroll->value(); a two-column spread's right-hand
    // page starts one page height further in, and a click there needs its
    // own x offset too - see paintPage's own two calls.
    int xOffset = 12;
    qint64 yBase = m_vscroll->value();
    if (m_readingMode == ReadingMode::Paged && m_pagedColumns == 2) {
        const int col2Start = 12 + contentWidth() + kPagedColumnGap;
        if (contentPos.x() >= col2Start) {
            xOffset = col2Start;
            yBase += pageHeight();
        }
    }

    const qint64 documentY = contentPos.y() + yBase;
    qint64 top = 0;
    const int spineIndex = chapterAtY(documentY, &top);
    if (spineIndex < 0)
        return result;
    result.spineIndex = spineIndex;

    const RenderedChapter *chapter = renderedChapter(spineIndex);
    if (!chapter || chapter->textRuns.isEmpty()) {
        result.offset = 0;
        return result;
    }

    const qreal localX = contentPos.x() - xOffset;
    const qreal localY = double(documentY - top);

    const HtmlRenderer::TextRun *best = nullptr;
    qreal bestDistance = std::numeric_limits<qreal>::max();
    for (const HtmlRenderer::TextRun &run : chapter->textRuns) {
        qreal dy = 0;
        if (localY < run.rect.top())
            dy = run.rect.top() - localY;
        else if (localY > run.rect.bottom())
            dy = localY - run.rect.bottom();
        qreal dx = 0;
        if (localX < run.rect.left())
            dx = run.rect.left() - localX;
        else if (localX > run.rect.right())
            dx = localX - run.rect.right();
        // Vertical mismatch dominates: a click below/above a line should
        // never prefer a run on the wrong line just because it is narrower.
        const qreal distance = dy * 1000.0 + dx;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &run;
        }
    }
    if (!best) {
        result.offset = 0;
        return result;
    }

    const QFontMetricsF metrics(best->font);
    qsizetype charIndex = best->text.size();
    qreal previousAdvance = 0;
    for (qsizetype i = 0; i <= best->text.size(); ++i) {
        const qreal advance = metrics.horizontalAdvance(best->text.left(i));
        if (best->rect.left() + advance >= localX) {
            const qreal mid = best->rect.left() + (previousAdvance + advance) / 2.0;
            charIndex = localX < mid ? qMax<qsizetype>(0, i - 1) : i;
            break;
        }
        previousAdvance = advance;
    }
    result.offset = best->offset + charIndex;
    return result;
}

bool EpubView::wordBoundsAt(int spineIndex, qsizetype offset, qsizetype *start, qsizetype *end) const
{
    const auto it = m_cache.constFind(spineIndex);
    if (it == m_cache.constEnd())
        return false;
    for (const HtmlRenderer::TextRun &run : it->textRuns) {
        if (offset < run.offset || offset > run.offset + run.text.size())
            continue;
        const qsizetype local = offset - run.offset;
        static const QRegularExpression wordRe(QStringLiteral("\\S+"));
        for (auto wordIt = wordRe.globalMatch(run.text); wordIt.hasNext();) {
            const QRegularExpressionMatch match = wordIt.next();
            if (local >= match.capturedStart() && local <= match.capturedEnd()) {
                if (start)
                    *start = run.offset + match.capturedStart();
                if (end)
                    *end = run.offset + match.capturedEnd();
                return true;
            }
        }
        if (start)
            *start = offset;
        if (end)
            *end = offset;
        return true;
    }
    return false;
}

QList<QRectF> EpubView::rectsForRange(int spineIndex, qsizetype start, qsizetype end) const
{
    QList<QRectF> rects;
    if (start >= end)
        return rects;
    const auto it = m_cache.constFind(spineIndex);
    if (it == m_cache.constEnd())
        return rects;
    for (const HtmlRenderer::TextRun &run : it->textRuns) {
        const qsizetype runStart = run.offset;
        const qsizetype runEnd = run.offset + run.text.size();
        if (runEnd <= start || runStart >= end)
            continue;
        const qsizetype localStart = qMax(start, runStart) - runStart;
        const qsizetype localEnd = qMin(end, runEnd) - runStart;
        const QFontMetricsF metrics(run.font);
        const qreal x0 = metrics.horizontalAdvance(run.text.left(localStart));
        const qreal x1 = metrics.horizontalAdvance(run.text.left(localEnd));
        rects << QRectF(run.rect.left() + x0, run.rect.top(), qMax(2.0, x1 - x0), run.rect.height());
    }
    return rects;
}

QString EpubView::plainTextForRange(int spineIndex, qsizetype start, qsizetype end) const
{
    QString text;
    if (start >= end)
        return text;
    const auto it = m_cache.constFind(spineIndex);
    if (it == m_cache.constEnd())
        return text;
    qsizetype expected = -1;
    for (const HtmlRenderer::TextRun &run : it->textRuns) {
        const qsizetype runStart = run.offset;
        const qsizetype runEnd = run.offset + run.text.size();
        if (runEnd <= start || runStart >= end)
            continue;
        if (expected >= 0 && runStart > expected)
            text += QLatin1Char(' ');
        const qsizetype localStart = qMax(start, runStart) - runStart;
        const qsizetype localEnd = qMin(end, runEnd) - runStart;
        text += run.text.mid(localStart, localEnd - localStart);
        expected = runEnd;
    }
    return text;
}

void EpubView::paintHighlightsAndSelection(QPainter &painter, int spineIndex, qint64 top, int xOffset) const
{
    for (const EpubHighlight &highlight : m_highlights) {
        if (highlight.locator.spineIndex != spineIndex)
            continue;
        QColor color = m_theme.color(QStringLiteral("--color-%1").arg(highlight.color));
        if (!color.isValid())
            color = QColor(highlight.color);
        color.setAlpha(90);
        for (const QRectF &rect : rectsForRange(spineIndex, highlight.locator.start, highlight.locator.end))
            painter.fillRect(rect.translated(xOffset, top), color);
    }
    if (m_selection.active && m_selection.spineIndex == spineIndex) {
        QColor color = m_theme.color(QStringLiteral("--text-accent"));
        color.setAlpha(80);
        for (const QRectF &rect : rectsForRange(spineIndex, m_selection.start, m_selection.end))
            painter.fillRect(rect.translated(xOffset, top), color);
    }
}

// --- mouse -----------------------------------------------------------------

bool EpubView::turnPageOnMarginClick(const QPoint &contentPos)
{
    if (m_readingMode != ReadingMode::Paged)
        return false;
    if (contentPos.x() < kMarginClickWidth) {
        turnPage(-1);
        return true;
    }
    if (contentPos.x() >= m_content->width() - kMarginClickWidth) {
        turnPage(1);
        return true;
    }
    return false;
}

void EpubView::handleContentMousePress(QMouseEvent *event)
{
    m_content->setFocus();
    if (event->button() != Qt::LeftButton)
        return;

    if (turnPageOnMarginClick(event->pos()))
        return; // click the margins to turn a page instead of starting a selection there

    const HitPoint hit = hitTest(event->pos());
    if (hit.spineIndex < 0)
        return;

    const int existing = highlightIndexAt(hit.spineIndex, hit.offset);
    if (existing >= 0) {
        editHighlightNote(existing);
        return;
    }

    qsizetype start = hit.offset;
    qsizetype end = hit.offset;
    wordBoundsAt(hit.spineIndex, hit.offset, &start, &end);
    m_dragAnchorPoint = hit;
    m_dragging = true;
    m_selection = {true, hit.spineIndex, start, end};
    m_content->update();
}

void EpubView::handleContentMouseMove(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const HitPoint hit = hitTest(event->pos());
    if (hit.spineIndex != m_dragAnchorPoint.spineIndex)
        return; // a selection never crosses a chapter boundary - see the report

    qsizetype anchorStart = m_dragAnchorPoint.offset;
    qsizetype anchorEnd = m_dragAnchorPoint.offset;
    wordBoundsAt(m_dragAnchorPoint.spineIndex, m_dragAnchorPoint.offset, &anchorStart, &anchorEnd);
    qsizetype hereStart = hit.offset;
    qsizetype hereEnd = hit.offset;
    wordBoundsAt(hit.spineIndex, hit.offset, &hereStart, &hereEnd);

    m_selection = {true, hit.spineIndex, qMin(anchorStart, hereStart), qMax(anchorEnd, hereEnd)};
    m_content->update();
}

void EpubView::handleContentMouseRelease(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_dragging = false;
}

void EpubView::handleContentDoubleClick(QMouseEvent *event)
{
    handleContentMousePress(event); // a press already selects the word under it
}

void EpubView::handleContentContextMenu(QContextMenuEvent *event)
{
    QMenu menu(m_content);

    if (m_selection.active && m_selection.end > m_selection.start) {
        for (const QString &color : EpubHighlights::colorNames()) {
            QAction *action = menu.addAction(tr("Highlight (%1)").arg(color));
            connect(action, &QAction::triggered, this, [this, color] { addHighlightFromSelection(color); });
        }
        menu.addSeparator();
        QAction *copyAction = menu.addAction(tr("Copy"));
        connect(copyAction, &QAction::triggered, this, [this] {
            QGuiApplication::clipboard()->setText(
                plainTextForRange(m_selection.spineIndex, m_selection.start, m_selection.end));
        });
    }

    const HitPoint hit = hitTest(event->pos());
    const int existing = hit.spineIndex >= 0 ? highlightIndexAt(hit.spineIndex, hit.offset) : -1;
    if (existing >= 0) {
        menu.addSeparator();
        QMenu *recolor = menu.addMenu(tr("Recolour"));
        for (const QString &color : EpubHighlights::colorNames()) {
            QAction *action = recolor->addAction(color);
            connect(action, &QAction::triggered, this, [this, existing, color] {
                if (existing < m_highlights.size()) {
                    m_highlights[existing].color = color;
                    persistHighlights();
                    m_content->update();
                }
            });
        }
        QAction *editAction = menu.addAction(tr("Edit note…"));
        connect(editAction, &QAction::triggered, this, [this, existing] { editHighlightNote(existing); });
        QAction *deleteAction = menu.addAction(tr("Delete highlight"));
        connect(deleteAction, &QAction::triggered, this, [this, existing] { removeHighlight(existing); });
    }

    if (!menu.isEmpty())
        menu.exec(event->globalPos());
}

// --- table of contents -------------------------------------------------

namespace {
/// Column 0 data roles a TOC row carries beyond Qt::UserRole (the spine
/// index): where to find this row's real
/// children once it is actually expanded, and whether that has happened yet
/// - see populateTocTree()/onTocExpanded().
constexpr int kTocEntryPointerRole = Qt::UserRole + 1;
constexpr int kTocPopulatedRole = Qt::UserRole + 2;
} // namespace

void EpubView::buildToc()
{
    // Not called from load() any more - see setTocVisible()'s own comment.
    // What runs here is itself lazy per node too: a synthetic
    // 2000-entry TOC used to cost roughly a third of open time built eagerly
    // three levels deep; now only the level actually being shown is ever
    // materialised as QTreeWidgetItems - see populateTocTree/onTocExpanded.
    m_tocTree->clear();
    populateTocTree(nullptr, m_document.tableOfContents());
    if (m_document.tableOfContents().isEmpty()) {
        // No NCX/nav at all: one row per spine item, numbered, so a book
        // with no declared TOC can still be jumped around in.
        for (int i = 0; i < m_document.spine().size(); ++i) {
            auto *item = new QTreeWidgetItem(m_tocTree, {tr("Chapter %1").arg(i + 1)});
            item->setData(0, Qt::UserRole, i);
        }
    }
}

void EpubView::populateTocTree(QTreeWidgetItem *parent, const QList<EpubTocEntry> &entries)
{
    for (const EpubTocEntry &entry : entries) {
        auto *item = parent ? new QTreeWidgetItem(parent, {entry.title})
                            : new QTreeWidgetItem(m_tocTree, {entry.title});
        item->setData(0, Qt::UserRole, entry.spineIndex);
        if (!entry.children.isEmpty()) {
            // `entry` is an element of a QList owned by m_document (either
            // tableOfContents() itself or some ancestor's `children`), never
            // resized or reallocated after EpubDocument::open() parses it -
            // stable for exactly as long as m_document stays loaded, which
            // is at least as long as this tree is (see load(), which clears
            // and rebuilds the whole tree - and this pointer - on every
            // open()). A single empty placeholder row is enough to make Qt
            // draw the expand arrow without walking `entry.children` now;
            // onTocExpanded() replaces it with the real rows the first time
            // this item is actually opened.
            item->setData(0, kTocEntryPointerRole, QVariant::fromValue<qintptr>(qintptr(&entry)));
            new QTreeWidgetItem(item, {QString()});
        }
    }
}

void EpubView::onTocActivated(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const int spineIndex = item->data(0, Qt::UserRole).toInt();
    if (spineIndex >= 0)
        jumpToSpine(spineIndex);
}

void EpubView::onTocExpanded(QTreeWidgetItem *item)
{
    if (!item || item->data(0, kTocPopulatedRole).toBool())
        return;
    item->setData(0, kTocPopulatedRole, true);
    const QVariant pointerValue = item->data(0, kTocEntryPointerRole);
    if (!pointerValue.isValid())
        return;
    const auto *entry = reinterpret_cast<const EpubTocEntry *>(pointerValue.value<qintptr>());
    qDeleteAll(item->takeChildren()); // drops the placeholder row buildToc()/an earlier expand left
    if (entry)
        populateTocTree(item, entry->children);
}

int EpubView::tocTopLevelCountForTest() const
{
    return m_tocTree->topLevelItemCount();
}

int EpubView::tocChildCountForTest(int topLevelIndex) const
{
    QTreeWidgetItem *item = m_tocTree->topLevelItem(topLevelIndex);
    return item ? item->childCount() : -1;
}

void EpubView::expandTocForTest(int topLevelIndex)
{
    if (QTreeWidgetItem *item = m_tocTree->topLevelItem(topLevelIndex))
        item->setExpanded(true); // synchronously emits itemExpanded -> onTocExpanded
}

void EpubView::setTocVisible(bool visible)
{
    m_tocVisible = visible;
    // buildToc() runs at most once per open(): the first time the panel is
    // actually shown, not at load() - a book whose reader never opens the
    // TOC pays nothing for it at all, and one who does pays only once (task
    // 2). load() already builds it early if the panel happens to still be
    // open across a book switch - see its own comment.
    if (visible && !m_tocBuilt) {
        buildToc();
        m_tocBuilt = true;
    }
    m_tocTree->setVisible(visible);
    if (m_tocButton->isChecked() != visible)
        m_tocButton->setChecked(visible);
}

// --- find ----------------------------------------------------------------

void EpubView::toggleFind()
{
    if (m_findBar->isHidden()) {
        m_findBar->show();
        m_findBar->raise();
        const QSize hint = m_findBar->sizeHint();
        m_findBar->setGeometry(m_content->width() - hint.width() - 12, 8, hint.width(), hint.height());
        m_findBar->focusSearch();
    } else {
        m_findBar->hide();
        m_content->setFocus();
    }
}

void EpubView::runFind(int direction)
{
    const QString pattern = m_findBar->pattern();
    if (pattern.isEmpty()) {
        m_findBar->setStatus(-1, 0, true);
        return;
    }
    const Qt::CaseSensitivity cs = m_findBar->caseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const int spineCount = m_document.spine().size();
    if (spineCount == 0)
        return;
    const int startSpine = m_findSpine >= 0 ? m_findSpine : chapterAtY(m_vscroll->value());
    const int step = direction >= 0 ? 1 : -1;

    for (int visited = 0; visited <= spineCount; ++visited) {
        const int spineIndex = ((startSpine + step * visited) % spineCount + spineCount) % spineCount;
        const RenderedChapter *chapter = renderedChapter(spineIndex);
        if (!chapter)
            continue;
        const QString text = chapterPlainText(*chapter);

        qsizetype from;
        if (visited == 0 && spineIndex == m_findSpine && m_findOffset >= 0)
            from = step > 0 ? m_findOffset + 1 : m_findOffset - 1;
        else
            from = step > 0 ? 0 : text.size();

        const qsizetype found = step > 0 ? text.indexOf(pattern, qMax<qsizetype>(0, from), cs)
                                         : text.lastIndexOf(pattern, qMin(from, qsizetype(text.size()) - 1), cs);
        if (found >= 0) {
            m_findSpine = spineIndex;
            m_findOffset = found;
            m_selection = {true, spineIndex, found, found + pattern.size()};
            jumpToSpine(spineIndex, topForOffset(*chapter, found));
            m_findBar->setStatus(0, 1, true);
            return;
        }
    }
    m_findBar->setStatus(-1, 0, true);
}

// --- highlights: CRUD + sidecar persistence -------------------------------

void EpubView::loadHighlights()
{
    Note sidecarNote;
    m_highlights = Vault::read(EpubHighlights::sidecarPath(m_document.path()), sidecarNote)
                      ? EpubHighlights::parse(sidecarNote.text)
                      : QList<EpubHighlight>();
}

void EpubView::persistHighlights()
{
    const QString bookPath = m_document.path();
    if (bookPath.isEmpty())
        return;

    const auto chapterTitle = [this](int spineIndex) -> QString {
        std::function<QString(const QList<EpubTocEntry> &)> search =
            [&](const QList<EpubTocEntry> &entries) -> QString {
            for (const EpubTocEntry &entry : entries) {
                if (entry.spineIndex == spineIndex && !entry.title.isEmpty())
                    return entry.title;
            }
            for (const EpubTocEntry &entry : entries) {
                const QString found = search(entry.children);
                if (!found.isEmpty())
                    return found;
            }
            return QString();
        };
        return search(m_document.tableOfContents());
    };

    QString error;
    if (!EpubHighlights::writeSidecar(EpubHighlights::sidecarPath(bookPath), QFileInfo(bookPath).fileName(),
                                      m_document.title(), m_highlights, chapterTitle, &error))
        emit statusMessage(tr("Could not write highlights: %1").arg(error), 4000);
}

int EpubView::highlightIndexAt(int spineIndex, qsizetype offset) const
{
    for (int i = 0; i < m_highlights.size(); ++i) {
        const EpubLocator &locator = m_highlights.at(i).locator;
        if (locator.spineIndex == spineIndex && offset >= locator.start && offset < locator.end)
            return i;
    }
    return -1;
}

void EpubView::addHighlightFromSelection(const QString &color)
{
    if (!m_selection.active || m_selection.end <= m_selection.start)
        return;
    EpubHighlight highlight;
    highlight.locator = {m_selection.spineIndex, m_selection.start, m_selection.end};
    highlight.color = color;
    highlight.quote = plainTextForRange(m_selection.spineIndex, m_selection.start, m_selection.end);
    m_highlights.append(highlight);
    persistHighlights();
    m_content->update();
}

void EpubView::removeHighlight(int index)
{
    if (index < 0 || index >= m_highlights.size())
        return;
    m_highlights.removeAt(index);
    persistHighlights();
    m_content->update();
}

void EpubView::editHighlightNote(int index)
{
    if (index < 0 || index >= m_highlights.size())
        return;
    bool ok = false;
    const QString text =
        QInputDialog::getMultiLineText(this, tr("Highlight note"),
                                       tr("Quote: “%1”").arg(m_highlights.at(index).quote),
                                       m_highlights.at(index).note, &ok);
    if (!ok || index >= m_highlights.size())
        return;
    m_highlights[index].note = text;
    persistHighlights();
}

// --- reading position ------------------------------------------------------

void EpubView::savePosition()
{
    if (m_document.path().isEmpty())
        return;
    qint64 top = 0;
    const int spineIndex = chapterAtY(m_vscroll->value(), &top);
    if (spineIndex < 0)
        return;
    EpubPosition position;
    position.spineIndex = spineIndex;
    position.scrollWithinChapter = int(m_vscroll->value() - top);
    position.fontPercent = m_fontPercent;
    position.paged = pagedMode();
    EpubPositionStore::set(m_document.path(), position);
}

void EpubView::restorePosition()
{
    const auto position = EpubPositionStore::get(m_document.path());
    if (!position)
        return;
    m_fontPercent = qBound(50, position->fontPercent, 300);
    if (pagedMode() != position->paged)
        applyReadingMode(position->paged); // silent - a restore, not a user action; setPagedMode() would re-save
    jumpToSpine(position->spineIndex, position->scrollWithinChapter);
}

} // namespace noteAppC
