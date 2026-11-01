// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

#include "EpubDocument.h"
#include "EpubHighlights.h"
#include "HtmlRenderer.h"
#include "Lifeline.h"
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

/// The EPUB reader: continuous scroll or paged through a book's
/// spine, each chapter laid out and rasterised by litehtml through
/// HtmlRenderer - the same renderer a note's raw HTML block already uses
/// (see HtmlRenderer.h for the two small things this reader needed from it
/// beyond a single block: the book's own stylesheet, and a text-run map to
/// turn a click into a character position). The layout/raster call itself
/// runs on a worker thread (requestChapterRender(), via the same
/// Lifeline+generation pattern MarkdownEditor's embeds use); resource
/// extraction, CSS collection and font-database registration stay on this
/// thread first - see requestChapterRender()'s own comment on why. Highlights
/// and notes live in a sidecar Obsidian note beside the book (EpubHighlights)
/// that this view both reads from, on open(), and writes to immediately on
/// every edit - a book's own bytes are never touched, and there is nothing
/// left pending for MainWindow's modified/save-prompt machinery to ask about.
///
/// Built as a plain QWidget with its own child QScrollBar, painting its
/// document area by hand - the same shape CanvasView already uses in this
/// codebase - rather than QAbstractScrollArea, so the header row (title, table
/// of contents toggle, font size, find, paged-mode toggle) and the
/// collapsible TOC tree sit beside the document naturally instead of fighting
/// that class's own single-viewport layout.
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
    /// own locator, the link form), a bare "<spine-index>", or a TOC
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

    // --- test hooks: paged mode, off-thread rendering, lazy TOC -----------
    bool pagedModeForTest() const { return pagedMode(); }
    void setPagedModeForTest(bool paged) { setPagedMode(paged); }
    void turnPageForTest(int direction) { turnPage(direction); }
    int pagedColumnsForTest() const { return m_pagedColumns; }
    qint64 scrollValueForTest() const;
    /// True once spineIndex's chapter has actually landed in the render
    /// cache - real content, not a placeholder (see RenderedChapter).
    bool chapterCachedForTest(int spineIndex) const { return m_cache.contains(spineIndex); }
    QImage chapterImageForTest(int spineIndex) const { return m_cache.value(spineIndex).image; }
    /// Kicks off (or joins) spineIndex's render exactly like paint would,
    /// without a real paint event.
    void requestChapterForTest(int spineIndex) { renderedChapter(spineIndex); }
    int pendingRenderCountForTest() const { return m_pendingSpine.size(); }
    /// True once the panel has actually been built (see setTocVisible()'s
    /// own comment on why that is deferred past load()).
    bool tocBuiltForTest() const { return m_tocBuilt; }
    void setTocVisibleForTest(bool visible) { setTocVisible(visible); }
    int tocTopLevelCountForTest() const;
    /// -1 if `topLevelIndex` is out of range.
    int tocChildCountForTest(int topLevelIndex) const;
    /// As if the reader opened this top-level row for the first time.
    void expandTocForTest(int topLevelIndex);
    /// How many distinct archive paths registerChapterFonts() has handed to
    /// QFontDatabase so far - success or not, see its own comment.
    int registeredFontCountForTest() const { return m_registeredFonts.size(); }
    QString cacheRootForTest() const { return m_cacheRoot; }
    int pageHeightForTest() const { return pageHeight(); }
    qint64 scrollMaximumForTest() const;
    int currentSpineIndexForTest();
    /// As if the reader clicked at (x, y) within the content area - the
    /// "click margins to turn" gesture; true (and a page already turned) for a
    /// margin click, false (nothing happened) anywhere else.
    bool marginClickForTest(int x, int y) { return turnPageOnMarginClick(QPoint(x, y)); }

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
    /// Cache hit, or nullptr while a render is in flight (or has not been
    /// asked for yet - this kicks it off). Never blocks: the litehtml
    /// layout/raster itself always runs on a worker thread now - see
    /// requestChapterRender()/deliverChapterRender() and the report on why
    /// that is safe. A caller painting a chapter that comes back null shows
    /// whatever height is already known (m_chapterHeights) as a placeholder
    /// and picks the image up on the repaint deliverChapterRender() triggers.
    const RenderedChapter *renderedChapter(int spineIndex);
    /// The old synchronous path, kept for the two callers that need a
    /// chapter's exact text-run offsets *right now* to answer a one-off user
    /// action (jump to a highlight/TOC target, find-in-book) rather than
    /// paint a frame - see navigateToFragment()/runFind(). Also what a
    /// worker's own request preparation still runs synchronously up to the
    /// point of dispatch (resource extraction, font registration - see the
    /// class comment on requestChapterRender for why those specifically stay
    /// on this thread).
    const RenderedChapter *renderedChapterBlocking(int spineIndex);
    /// Starts (or joins, if already pending) an off-thread render of
    /// `spineIndex` and returns immediately. Resource extraction, CSS
    /// collection and any `@font-face` registration happen here, synchronously,
    /// before the request is handed to the worker - see the class comment.
    void requestChapterRender(int spineIndex);
    /// Delivered on the UI thread via m_lifeline. Stale (superseded by a
    /// resize/font-size/theme change since the request went out - see
    /// m_renderEpoch) results are dropped rather than cached.
    void deliverChapterRender(int spineIndex, quint64 epoch, QImage image,
                              QList<HtmlRenderer::TextRun> textRuns);
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
    /// Extracts `archivePath` to resourceCachePath() if not already there;
    /// empty on failure (unreadable entry, or the path escaped the archive -
    /// see EpubDocument::resolveArchivePath). Shared by every caller that
    /// turns an archive-relative reference into a real file litehtml/Qt can
    /// open: image `src`, CSS `url()`, and a `@font-face` source's bytes.
    QString extractToCache(const QString &archivePath) const;
    /// Extracts every local `<img src>`/`<image xlink:href>` the chapter
    /// references (skipped if already cached) and rewrites those attributes
    /// to the extracted absolute path.
    QString rewriteResourceRefs(QString html, const QString &chapterArchivePath);
    /// `<link rel="stylesheet">` and inline `<style>` text collected from the
    /// chapter, with every `url()` already rewritten (rewriteCssUrls) and
    /// every `@font-face` it declares registered (registerChapterFonts) -
    /// see both for why this has to run before HtmlRenderer::render ever
    /// sees the result, not inside it.
    QString collectStylesheets(const QString &html, const QString &chapterArchivePath);
    /// Rewrites every `url(...)` in `css` - a background image, or a
    /// `@font-face` source litehtml itself never reads (harmless either way)
    /// - resolved against `stylesheetArchivePath` (the *stylesheet's* own
    /// path, per CSS's own resolution rule, not the chapter's) and
    /// extracted the same way rewriteResourceRefs extracts an `<img>`.
    /// Remote and `data:` URLs are left untouched (never fetched, same rule
    /// as everywhere else in this reader).
    QString rewriteCssUrls(const QString &css, const QString &stylesheetArchivePath) const;
    /// Registers every `@font-face` source this stylesheet declares into the
    /// application font database, at most once per archive path for the
    /// life of this process (m_registeredFonts) - so a book's own typeface
    /// actually renders instead of falling back to the theme's font. Must
    /// run on the Qt GUI thread: QFontDatabase::addApplicationFont(FromData)
    /// is not safe off it, the same rule that keeps MathRenderer's font
    /// loading on Publisher's calling thread rather than its worker pool
    /// (see PageBody.h/Publisher.cpp) - which is exactly why this is called
    /// from collectStylesheets(), still on the thread that called
    /// requestChapterRender(), never from the litehtml worker itself. A
    /// format Qt cannot load (most books ship WOFF, which Qt does not) fails
    /// silently and degrades to the theme's font, the same "nothing draws"
    /// rule a missing/remote image already gets.
    void registerChapterFonts(const QString &css, const QString &stylesheetArchivePath);
    HtmlRenderer::Theme rendererTheme() const;
    /// The width litehtml lays a chapter out to: the full content area in
    /// continuous mode and single-column paged mode, half of it (minus the
    /// inter-column gap) in two-column paged mode - see recomputeColumns().
    int contentWidth() const;
    void invalidateRenders(); // font size / width / theme changed: drop every cached image

    void ensureHeightsSized();       // m_chapterHeights.size() == spine().size()
    qint64 chapterTop(int spineIndex);     // sums known/guessed chapter heights, never renders
    qint64 documentHeight();
    int chapterAtY(qint64 documentY, qint64 *topOut = nullptr);
    void updateScrollRange();
    void paintContent(QPainter &painter);

    // --- paged mode: EpubRender.cpp -----------------------------------------
    enum class ReadingMode { Continuous, Paged };
    /// One page is exactly one content-viewport-height slice of the same
    /// document-space continuous mode already scrolls through (see the class
    /// comment); paged mode just snaps to multiples of this and shows one or
    /// two of them at once instead of scrolling smoothly. That reuse is what
    /// keeps hit-testing, highlight painting and the render cache identical
    /// in both modes - see the report.
    int pageHeight() const;
    qint64 snappedPageTop(qint64 documentY) const;
    /// True/false column count for the *current* width, recomputed on
    /// resize and on entering/leaving paged mode; 1 below kTwoColumnMinWidth.
    void recomputeColumns();
    void paintPaged(QPainter &painter);
    /// Paints one page's worth of content (from document-Y `pageDocTop`) at
    /// horizontal offset `xOffset` - called once for a single page, twice
    /// (left/right) for a two-column spread.
    void paintPage(QPainter &painter, qint64 pageDocTop, int xOffset);
    void turnPage(int direction); // +1/-1 pages (2 at a time in two-column mode)
    /// Shared by setPagedMode() and restorePosition(): mode/column-count
    /// bookkeeping common to both, without the position save the first does
    /// and the second must not (see either call site).
    void applyReadingMode(bool paged);

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
    void paintHighlightsAndSelection(QPainter &painter, int spineIndex, qint64 top, int xOffset = 12) const;
    QList<QRectF> rectsForRange(int spineIndex, qsizetype start, qsizetype end) const;
    QString plainTextForRange(int spineIndex, qsizetype start, qsizetype end) const;

    void handleContentMousePress(QMouseEvent *event);
    void handleContentMouseMove(QMouseEvent *event);
    void handleContentMouseRelease(QMouseEvent *event);
    void handleContentDoubleClick(QMouseEvent *event);
    void handleContentContextMenu(QContextMenuEvent *event);
    /// Paged mode only: true (and a page already turned) when `contentPos`
    /// falls in the left/right margin strip rather than the text - the
    /// "click margins to turn" gesture.
    bool turnPageOnMarginClick(const QPoint &contentPos);

    void buildToc();
    /// One level of the tree at a time: every entry in `entries` gets its
    /// row, but one with children gets a single empty placeholder child
    /// instead of recursing - onTocExpanded() replaces it with the real
    /// grandchildren the first time that row is actually opened. Bounds a
    /// huge TOC's cost to the rows actually shown rather than the whole tree
    /// - see buildToc()'s own comment on why this is also never called at
    /// load() at all.
    void populateTocTree(QTreeWidgetItem *parent, const QList<EpubTocEntry> &entries);
    void onTocActivated(QTreeWidgetItem *item);
    void onTocExpanded(QTreeWidgetItem *item);
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
    void setPagedMode(bool paged);
    bool pagedMode() const { return m_readingMode == ReadingMode::Paged; }

    EpubDocument m_document;
    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    QString m_cacheRoot;

    int m_fontPercent = 100;
    QHash<int, RenderedChapter> m_cache;
    QList<int> m_cacheOrder; ///< most-recently-used last
    static constexpr int kMaxCachedChapters = 7;
    static constexpr int kHeaderHeight = 34;
    static constexpr int kTocWidth = 240;
    static constexpr int kTwoColumnMinWidth = 900; ///< below this, paged mode falls back to one column
    static constexpr int kPagedColumnGap = 24;
    static constexpr int kMarginClickWidth = 44; ///< click inside this many px of either edge to turn

    QList<qint64> m_chapterHeights;
    QList<bool> m_chapterHeightExact;

    // --- off-thread chapter rendering ------------------------------
    std::shared_ptr<Lifeline> m_lifeline = std::make_shared<Lifeline>();
    QSet<int> m_pendingSpine; ///< spine indices with a worker render currently in flight
    /// Bumped by invalidateRenders() (resize, font size, theme, paged/column
    /// change) and by load(); a delivered render whose captured epoch no
    /// longer matches is stale (the width/theme it was rendered for is gone)
    /// and is dropped rather than cached - see deliverChapterRender().
    quint64 m_renderEpoch = 0;
    QSet<QString> m_registeredFonts; ///< archive paths already handed to QFontDatabase, tried at most once

    // --- lazy table of contents ------------------------------------
    bool m_tocBuilt = false; ///< buildToc() deferred until the panel is first shown - see setTocVisible()

    // --- paged mode -------------------------------------------------
    ReadingMode m_readingMode = ReadingMode::Continuous;
    int m_pagedColumns = 1;

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
    QToolButton *m_pagedButton = nullptr;
    QTreeWidget *m_tocTree = nullptr;
    bool m_tocVisible = false;
    FindBar *m_findBar = nullptr;
    int m_findSpine = -1;
    qsizetype m_findOffset = -1;
    QTimer *m_positionSaveTimer = nullptr; ///< debounces savePosition() while the scrollbar is dragged
};

} // namespace noteAppC
