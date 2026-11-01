#include "EpubView.h"

#include "FindBar.h"
#include "Icons.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace noteAppC {

EpubView::EpubView(QWidget *parent)
    : QWidget(parent)
    , m_theme(Theme::builtin())
{
    setObjectName(QStringLiteral("epubView"));
    setFocusPolicy(Qt::StrongFocus);
    buildChrome();
}

EpubView::~EpubView()
{
    // First thing: no worker's post() can land on this object past this
    // point (see Lifeline), so everything below runs exactly as if no
    // render were ever in flight.
    m_lifeline->end();
    if (m_document.isValid())
        savePosition();
}

void EpubView::buildChrome()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // --- header: title, TOC toggle, font size, find, extract-highlights ---
    m_header = new QWidget(this);
    // Background painted by hand in the eventFilter (see applyThemeToChrome's
    // comment on why, not just a QSS `background:` rule).
    m_header->installEventFilter(this);
    m_header->setFixedHeight(kHeaderHeight);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(8, 4, 8, 4);

    m_tocButton = new QToolButton(m_header);
    m_tocButton->setProperty("iconName", QStringLiteral("list")); // drawn in applyThemeToChrome
    m_tocButton->setCheckable(true);
    m_tocButton->setToolTip(tr("Table of contents"));
    connect(m_tocButton, &QToolButton::toggled, this, &EpubView::setTocVisible);
    headerLayout->addWidget(m_tocButton);

    m_titleLabel = new QLabel(m_header);
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
    headerLayout->addWidget(m_titleLabel, 1);

    m_fontDownButton = new QToolButton(m_header);
    m_fontDownButton->setText(QStringLiteral("A-"));
    m_fontDownButton->setToolTip(tr("Smaller text"));
    connect(m_fontDownButton, &QToolButton::clicked, this, [this] { changeFontPercent(-10); });
    headerLayout->addWidget(m_fontDownButton);

    m_fontUpButton = new QToolButton(m_header);
    m_fontUpButton->setText(QStringLiteral("A+"));
    m_fontUpButton->setToolTip(tr("Larger text"));
    connect(m_fontUpButton, &QToolButton::clicked, this, [this] { changeFontPercent(10); });
    headerLayout->addWidget(m_fontUpButton);

    m_findButton = new QToolButton(m_header);
    m_findButton->setProperty("iconName", QStringLiteral("search"));
    m_findButton->setToolTip(tr("Find in book (Ctrl+F)"));
    connect(m_findButton, &QToolButton::clicked, this, &EpubView::toggleFind);
    headerLayout->addWidget(m_findButton);

    // Text, not an icon - same call as m_extractButton below: a plain label
    // needs no Lucide name guessed against the bundled set (see Icons.h).
    m_pagedButton = new QToolButton(m_header);
    m_pagedButton->setText(tr("Paged"));
    m_pagedButton->setCheckable(true);
    m_pagedButton->setToolTip(
        tr("Read in pages instead of scrolling (Page Up/Down, arrow keys, click the margins)"));
    connect(m_pagedButton, &QToolButton::toggled, this, &EpubView::setPagedMode);
    headerLayout->addWidget(m_pagedButton);

    m_extractButton = new QToolButton(m_header);
    m_extractButton->setText(tr("Extract highlights"));
    m_extractButton->setToolTip(tr("Rewrite this book's sidecar note from the highlights below"));
    connect(m_extractButton, &QToolButton::clicked, this, &EpubView::persistHighlights);
    headerLayout->addWidget(m_extractButton);

    outer->addWidget(m_header);

    // --- body: an optional TOC tree beside the document area --------------
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_tocTree = new QTreeWidget(this);
    m_tocTree->setHeaderHidden(true);
    m_tocTree->setFixedWidth(kTocWidth);
    m_tocTree->setVisible(false);
    connect(m_tocTree, &QTreeWidget::itemClicked, this, &EpubView::onTocActivated);
    connect(m_tocTree, &QTreeWidget::itemExpanded, this, &EpubView::onTocExpanded);
    body->addWidget(m_tocTree);

    // The document area and its scrollbar: painted and hit-tested by hand
    // (see EpubRender.cpp/EpubSelection.cpp), the same shape CanvasView
    // already uses in this codebase - see the class comment.
    m_content = new QWidget(this);
    m_content->setMouseTracking(true);
    m_content->setFocusPolicy(Qt::StrongFocus);
    m_content->installEventFilter(this);
    body->addWidget(m_content, 1);

    m_vscroll = new QScrollBar(Qt::Vertical, this);
    m_positionSaveTimer = new QTimer(this);
    m_positionSaveTimer->setSingleShot(true);
    connect(m_positionSaveTimer, &QTimer::timeout, this, &EpubView::savePosition);
    connect(m_vscroll, &QScrollBar::valueChanged, this, [this](int) {
        m_content->update();
        m_positionSaveTimer->start(500); // debounced: not every pixel of a drag
    });
    body->addWidget(m_vscroll);

    outer->addLayout(body, 1);

    m_findBar = new FindBar(m_content);
    m_findBar->setReplaceShown(false);
    m_findBar->hide();
    connect(m_findBar, &FindBar::queryChanged, this, [this] { runFind(0); });
    connect(m_findBar, &FindBar::stepped, this, &EpubView::runFind);
    connect(m_findBar, &FindBar::dismissed, this, [this] { m_findBar->hide(); m_content->setFocus(); });

    applyThemeToChrome();
}

void EpubView::setTheme(const Theme &theme)
{
    m_theme = theme;
    invalidateRenders();
    applyThemeToChrome();
    m_content->update();
}

void EpubView::applyThemeToChrome()
{
    const QColor background = m_theme.color(QStringLiteral("--background-secondary"));
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));
    const QColor border = m_theme.color(QStringLiteral("--background-modifier-border"));
    // Only `color:` here - inherited by Qt's stylesheet cascade the way real
    // CSS inherits text colour. The background is painted by hand in the
    // eventFilter's QEvent::Paint case below instead of through a `background:`
    // rule: a bare QWidget's stylesheet background did not reliably repaint
    // after a *second* setTheme() call in testing (screenshot evidence, not
    // just Qt's usual WA_StyledBackground gotcha - that was set and still did
    // not help), where m_content's own hand-painted fill (paintContent()) always
    // tracked the theme correctly. Same fix, applied here too.
    m_header->setStyleSheet(QStringLiteral("color: %1;").arg(text.name()));
    m_tocTree->setStyleSheet(QStringLiteral("background: %1; color: %2; border-right: 1px solid %3;")
                                 .arg(background.name(), text.name(), border.name()));
    m_content->setStyleSheet(QStringLiteral("background: %1;")
                                 .arg(m_theme.color(QStringLiteral("--background-primary")).name()));
    // Flat and explicitly coloured, not left to the application's ambient
    // QPalette (normally MainWindow's job - see CanvasView's own toolbar-less
    // chrome - but this widget can be shown before that pass ever runs, a
    // screenshot tool being exactly that case, and looks wrong meanwhile).
    const QString buttonCss = QStringLiteral("QToolButton { background: transparent; color: %1; border: "
                                             "1px solid %2; border-radius: 3px; padding: 2px 6px; } "
                                             "QToolButton:checked, QToolButton:pressed { background: %3; }")
                                  .arg(text.name(), border.name(),
                                       m_theme.color(QStringLiteral("--background-modifier-hover")).name());
    for (QToolButton *button :
        {m_tocButton, m_fontDownButton, m_fontUpButton, m_findButton, m_extractButton, m_pagedButton}) {
        button->setStyleSheet(buttonCss);
        // The app's own Lucide icons, in the text's colour, as everywhere else.
        if (const QString icon = button->property("iconName").toString(); !icon.isEmpty())
            button->setIcon(Icons::icon(icon, text, 16));
    }
    m_header->update();
    if (m_findBar)
        m_findBar->setTheme(m_theme);
}

bool EpubView::load(const QString &path, QString *error)
{
    if (!m_document.open(path, error))
        return false;

    ++m_renderEpoch; // a render still in flight for the previous book must never land in this one's cache
    m_pendingSpine.clear();
    m_cache.clear();
    m_cacheOrder.clear();
    m_selection = Selection{};
    m_cacheRoot = cacheRootForBook();

    ensureHeightsSized();
    // buildToc() is deferred to the panel's first show - see setTocVisible()
    // and its own comment (a book with a huge TOC pays nothing for
    // it at open unless the reader actually opens the panel).
    m_tocTree->clear();
    m_tocBuilt = false;
    if (m_tocVisible) {
        buildToc();
        m_tocBuilt = true;
    }
    loadHighlights();
    restorePosition();

    m_titleLabel->setText(m_document.title().isEmpty() ? QFileInfo(path).fileName() : m_document.title());
    m_titleLabel->setToolTip(m_document.author().isEmpty()
                                 ? m_titleLabel->text()
                                 : tr("%1 — %2").arg(m_titleLabel->text(), m_document.author()));

    updateScrollRange();
    m_content->update();
    return true;
}

bool EpubView::navigateToFragment(const QString &fragment)
{
    if (fragment.isEmpty()) {
        jumpToSpine(0);
        return true;
    }
    if (const auto locator = EpubLocator::parse(fragment)) {
        if (locator->spineIndex >= 0 && locator->spineIndex < m_document.spine().size()) {
            // Blocking: a highlight/TOC jump is a one-off user action, not
            // the scroll/open hot path, and it needs the
            // exact offset->Y position right now to land the view on the
            // right line rather than just the chapter's top - see the
            // header comment on renderedChapterBlocking().
            const RenderedChapter *chapter = renderedChapterBlocking(locator->spineIndex);
            qint64 withinY = 0;
            if (chapter) {
                for (const HtmlRenderer::TextRun &run : chapter->textRuns) {
                    if (run.offset + run.text.size() > locator->start) {
                        withinY = qint64(run.rect.top());
                        break;
                    }
                }
            }
            jumpToSpine(locator->spineIndex, withinY);
            m_selection = {true, locator->spineIndex, locator->start, locator->end};
            m_content->update();
            return true;
        }
    }

    bool ok = false;
    const int spineIndex = fragment.toInt(&ok);
    if (ok && spineIndex >= 0 && spineIndex < m_document.spine().size()) {
        jumpToSpine(spineIndex);
        return true;
    }

    // A TOC/element id: search the manifest order starting from the current
    // chapter's own file first (an in-chapter footnote link, the common
    // case), then every chapter's raw markup for id="<fragment>" or
    // name="<fragment>" - litehtml has no live DOM once rasterised, so this
    // is a text search over each chapter's source rather than an element
    // lookup; good enough for jumping, not for centring on the exact pixel.
    for (int i = 0; i < m_document.spine().size(); ++i) {
        QString entryError;
        const QByteArray raw = m_document.readEntry(m_document.spine().at(i).archivePath, &entryError);
        if (raw.isEmpty())
            continue;
        const QString html = QString::fromUtf8(raw);
        if (html.contains(QStringLiteral("id=\"%1\"").arg(fragment))
            || html.contains(QStringLiteral("name=\"%1\"").arg(fragment))) {
            jumpToSpine(i);
            return true;
        }
    }
    return false;
}

void EpubView::jumpToSpine(int spineIndex, qint64 withinChapterY)
{
    if (spineIndex < 0 || spineIndex >= m_document.spine().size())
        return;
    const qint64 top = chapterTop(spineIndex);
    updateScrollRange();
    qint64 target = qBound<qint64>(0, top + withinChapterY, qint64(m_vscroll->maximum()));
    if (m_readingMode == ReadingMode::Paged)
        target = snappedPageTop(target); // land on a page boundary, not mid-page - see pageHeight()'s comment
    m_vscroll->setValue(int(target));
    m_content->update();
}

void EpubView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_findBar && !m_findBar->isHidden()) {
        const QSize hint = m_findBar->sizeHint();
        m_findBar->setGeometry(m_content->width() - hint.width() - 12, 8, hint.width(), hint.height());
    }
}

bool EpubView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(m_header);
            painter.fillRect(m_header->rect(), m_theme.color(QStringLiteral("--background-secondary")));
            painter.setPen(m_theme.color(QStringLiteral("--background-modifier-border")));
            painter.drawLine(0, m_header->height() - 1, m_header->width() - 1, m_header->height() - 1);
            return true;
        }
        return false;
    }
    if (watched != m_content)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Paint: {
        QPainter painter(m_content);
        paintContent(painter);
        return true;
    }
    case QEvent::Resize:
        recomputeColumns(); // may cross kTwoColumnMinWidth
        invalidateRenders(); // wrap width changed: every cached raster is stale
        updateScrollRange();
        if (m_findBar && !m_findBar->isHidden()) {
            const QSize hint = m_findBar->sizeHint();
            m_findBar->setGeometry(m_content->width() - hint.width() - 12, 8, hint.width(), hint.height());
        }
        return false;
    case QEvent::MouseButtonPress:
        handleContentMousePress(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::MouseMove:
        handleContentMouseMove(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::MouseButtonRelease:
        handleContentMouseRelease(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::MouseButtonDblClick:
        handleContentDoubleClick(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::ContextMenu:
        handleContentContextMenu(static_cast<QContextMenuEvent *>(event));
        return true;
    case QEvent::Wheel:
        wheelEvent(static_cast<QWheelEvent *>(event));
        return true;
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy) && m_selection.active) {
            QGuiApplication::clipboard()->setText(
                plainTextForRange(m_selection.spineIndex, m_selection.start, m_selection.end));
            return true;
        }
        if (key->matches(QKeySequence::Find)) {
            toggleFind();
            return true;
        }
        if (key->key() == Qt::Key_PageDown) {
            if (m_readingMode == ReadingMode::Paged)
                turnPage(1);
            else
                m_vscroll->setValue(m_vscroll->value() + m_vscroll->pageStep());
            return true;
        }
        if (key->key() == Qt::Key_PageUp) {
            if (m_readingMode == ReadingMode::Paged)
                turnPage(-1);
            else
                m_vscroll->setValue(m_vscroll->value() - m_vscroll->pageStep());
            return true;
        }
        // Arrow keys turn a page only in paged mode. Continuous
        // mode leaves them unhandled.
        if (m_readingMode == ReadingMode::Paged
            && (key->key() == Qt::Key_Right || key->key() == Qt::Key_Down)) {
            turnPage(1);
            return true;
        }
        if (m_readingMode == ReadingMode::Paged
            && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Up)) {
            turnPage(-1);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

void EpubView::wheelEvent(QWheelEvent *event)
{
    const QPointF delta = event->pixelDelta().isNull() ? QPointF(event->angleDelta()) / 2.0
                                                        : QPointF(event->pixelDelta());
    if (m_readingMode == ReadingMode::Paged) {
        // A notch turns a whole page rather than scrolling it smoothly - the
        // way every other paged reader treats the wheel. angleDelta's own
        // "one notch" is 120 per Qt's docs; pixelDelta (a touchpad) rarely
        // reports one, so either direction of a small nudge still turns
        // exactly one page rather than none.
        if (delta.y() < 0)
            turnPage(1);
        else if (delta.y() > 0)
            turnPage(-1);
        event->accept();
        return;
    }
    m_vscroll->setValue(m_vscroll->value() - int(delta.y()));
    event->accept();
}

void EpubView::changeFontPercent(int delta)
{
    m_fontPercent = qBound(50, m_fontPercent + delta, 300);
    invalidateRenders();
    updateScrollRange();
    m_content->update();
    savePosition();
}

void EpubView::setPagedMode(bool paged)
{
    if (pagedMode() == paged)
        return;
    applyReadingMode(paged);
    savePosition(); // a user action (the header button) - restorePosition() applies the same mode silently
}

} // namespace noteAppC
