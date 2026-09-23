#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

#include "EpubDocument.h"
#include "EpubHighlights.h"
#include "HtmlRenderer.h"
#include "Theme.h"

class QLabel;
class QPainter;
class QScrollBar;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace noteAppC {

class FindBar;

/// The EPUB reader: continuous scroll through a book's spine, each chapter
/// laid out and rasterised by litehtml through HtmlRenderer - the same
/// renderer a note's raw HTML block already uses (see HtmlRenderer.h for the
/// two small things this reader needed from it beyond a single block: the
/// book's own stylesheet, and a text-run map to turn a click into a
/// character position). Highlights and notes live in a sidecar Obsidian note
/// beside the book (EpubHighlights) that this view both reads from, on
/// open(), and writes to immediately on every edit - a book's own bytes are
/// never touched, and there is nothing left pending for MainWindow's
/// modified/save-prompt machinery to ask about.
///
/// Built as a plain QWidget with its own child QScrollBar, painting its
/// document area by hand - the same shape CanvasView already uses in this
/// codebase - rather than QAbstractScrollArea, so the header row (title, table
/// of contents toggle, font size, find) and the collapsible TOC tree sit
/// beside the document naturally instead of fighting that class's own
/// single-viewport layout.
class EpubView : public QWidget {
    Q_OBJECT

public:
    explicit EpubView(QWidget *parent = nullptr);
    ~EpubView() override;

    void setTheme(const Theme &theme);
    const Theme &theme() const { return m_theme; }

    bool load(const QString &path, QString *error = nullptr);
    QString path() const { return m_document.path(); }
    QString title() const { return m_document.title(); }

    /// Scrolls to a fragment - "epub:<spine>:<start>-<end>" (a highlight's
    /// own locator, task 3's link form), a bare "<spine-index>", or a TOC
    /// target's element id (searched for in the nearest chapter as a plain
    /// id="..."/name="..." attribute match, litehtml having no DOM to query
    /// live once rasterised - see the .cpp). False when nothing in the book
    /// matches.
    bool navigateToFragment(const QString &fragment);

    /// Always false: every highlight/note edit writes the sidecar
    /// immediately (see the class comment), so nothing is ever pending for
    /// MainWindow::isModified()/maybeSaveAll() to catch - which is also why
    /// neither needs a case added for this view at all (see the report).
    bool isModified() const { return false; }

    /// Test hook: the book's highlights as currently held (loaded from the
    /// sidecar on open(), kept live thereafter).
    const QList<EpubHighlight> &highlights() const { return m_highlights; }

signals:
    /// A link the book's own spine cannot satisfy - handed to
    /// MainWindow::openRelative exactly like every other pane's
    /// noteActivated (a vault-relative path, already resolved).
    void noteActivated(const QString &relativePath);
    void statusMessage(const QString &text, int timeoutMs);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // --- EpubView.cpp: chrome, theme, load, navigation ----------------------
    void buildChrome();
    void applyThemeToChrome();
    void jumpToSpine(int spineIndex, qint64 withinChapterY = 0);

    // --- EpubRender.cpp: chapters, caching, images, layout ------------------
    struct RenderedChapter {
        QImage image;
        QList<HtmlRenderer::TextRun> textRuns;
        qsizetype textLength = 0;
        int height = 0; ///< logical px, == image.height()/devicePixelRatio
    };
    const RenderedChapter *renderedChapter(int spineIndex);
    void evictFarChapters(int aroundSpineIndex);
    QString cacheRootForBook() const;
    /// Where a resource at `archivePath` (an image referenced by a chapter,
    /// read from inside the .epub) is extracted to on local disk, mirroring
    /// its archive path under the book's cache root - so two chapters
    /// referencing the same shared image share one cached file, and
    /// resolveImagePath's own relative-path math (HtmlRenderer.cpp) never
    /// enters into it: every src this class hands HtmlRenderer is already
    /// this absolute path.
    QString resourceCachePath(const QString &archivePath) const;
    /// Extracts every local `<img src>`/`<image xlink:href>` the chapter
    /// references (skipped if already cached) and rewrites those attributes
    /// to the extracted absolute path.
    QString rewriteResourceRefs(QString html, const QString &chapterArchivePath);
    QString collectStylesheets(const QString &html, const QString &chapterArchivePath) const;
    HtmlRenderer::Theme rendererTheme() const;
    int contentWidth() const;
    void invalidateRenders(); // font size / width / theme changed: drop every cached image

    void ensureHeightsSized();       // m_chapterHeights.size() == spine().size()
    qint64 chapterTop(int spineIndex);     // lays out (renders) chapters on demand to learn real heights
    qint64 documentHeight();
    int chapterAtY(qint64 documentY, qint64 *topOut = nullptr);
    void updateScrollRange();
    void paintContent(QPainter &painter);

    // --- EpubSelection.cpp: hit testing, selection, highlights, TOC, find ---
    struct HitPoint {
        int spineIndex = -1;
        qsizetype offset = -1;
    };
    HitPoint hitTest(const QPoint &contentPos);
    bool wordBoundsAt(int spineIndex, qsizetype offset, qsizetype *start, qsizetype *end) const;
    /// The reading-order plain text of a rendered chapter, reconstructed
    /// from its run map exactly the way HtmlRenderer built the offsets in
    /// the first place (see TextRun's own comment) - what find-in-book
    /// searches.
    static QString chapterPlainText(const RenderedChapter &chapter);
    static qint64 topForOffset(const RenderedChapter &chapter, qsizetype offset);
    void paintHighlightsAndSelection(QPainter &painter, int spineIndex, qint64 top) const;
    QList<QRectF> rectsForRange(int spineIndex, qsizetype start, qsizetype end) const;
    QString plainTextForRange(int spineIndex, qsizetype start, qsizetype end) const;

    void handleContentMousePress(QMouseEvent *event);
    void handleContentMouseMove(QMouseEvent *event);
    void handleContentMouseRelease(QMouseEvent *event);
    void handleContentDoubleClick(QMouseEvent *event);
    void handleContentContextMenu(QContextMenuEvent *event);

    void buildToc();
    void populateTocTree(QTreeWidgetItem *parent, const QList<EpubTocEntry> &entries);
    void onTocActivated(QTreeWidgetItem *item);
    void setTocVisible(bool visible);

    void toggleFind();
    void runFind(int direction);

    void loadHighlights();
    void persistHighlights();
    void addHighlightFromSelection(const QString &color);
    int highlightIndexAt(int spineIndex, qsizetype offset) const;
    void removeHighlight(int index);
    void editHighlightNote(int index);

    void savePosition();
    void restorePosition();
    void changeFontPercent(int delta);

    EpubDocument m_document;
    Theme m_theme;
    QString m_cacheRoot;

    int m_fontPercent = 100;
    QHash<int, RenderedChapter> m_cache;
    QList<int> m_cacheOrder; ///< most-recently-used last
    static constexpr int kMaxCachedChapters = 7;
    static constexpr int kHeaderHeight = 34;
    static constexpr int kTocWidth = 240;

    QList<qint64> m_chapterHeights;
    QList<bool> m_chapterHeightExact;

    QList<EpubHighlight> m_highlights;
    QString m_pendingHighlightColor = QStringLiteral("yellow");

    struct Selection {
        bool active = false;
        int spineIndex = -1;
        qsizetype start = 0;
        qsizetype end = 0;
    } m_selection;
    bool m_dragging = false;
    HitPoint m_dragAnchorPoint;

    QWidget *m_content = nullptr;
    QScrollBar *m_vscroll = nullptr;
    QWidget *m_header = nullptr;
    QLabel *m_titleLabel = nullptr;
    QToolButton *m_tocButton = nullptr;
    QToolButton *m_fontDownButton = nullptr;
    QToolButton *m_fontUpButton = nullptr;
    QToolButton *m_findButton = nullptr;
    QToolButton *m_extractButton = nullptr;
    QTreeWidget *m_tocTree = nullptr;
    bool m_tocVisible = false;
    FindBar *m_findBar = nullptr;
    int m_findSpine = -1;
    qsizetype m_findOffset = -1;
    QTimer *m_positionSaveTimer = nullptr; ///< debounces savePosition() while the scrollbar is dragged
};

} // namespace noteAppC
