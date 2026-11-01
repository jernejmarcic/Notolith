#include "EpubView.h"
#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QToolButton>
#include <QtConcurrentRun>

/// Chapters: off-thread litehtml rendering through HtmlRenderer, the
/// image/text-run cache that bounds memory on a big book, resource
/// extraction and font registration from the archive, and the
/// document-space layout (cumulative chapter heights) both continuous scroll
/// and paged mode are built on.

namespace noteAppC {

namespace {

QFont scaledFont(QFont font, int percent)
{
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(1, font.pixelSize() * percent / 100));
    else
        font.setPointSizeF(qMax(1.0, font.pointSizeF() * percent / 100.0));
    return font;
}

} // namespace

HtmlRenderer::Theme EpubView::rendererTheme() const
{
    HtmlRenderer::Theme theme;
    theme.text = m_theme.color(QStringLiteral("--text-normal"));
    theme.background = m_theme.color(QStringLiteral("--background-primary"));
    theme.link = m_theme.color(QStringLiteral("--text-accent"));
    theme.border = m_theme.color(QStringLiteral("--background-modifier-border"));
    theme.codeBackground = m_theme.color(QStringLiteral("--background-primary-alt"));
    theme.textFont = scaledFont(m_theme.textFont(), m_fontPercent);
    theme.codeFont = scaledFont(m_theme.monospaceFont(), m_fontPercent);
    return theme;
}

int EpubView::contentWidth() const
{
    if (m_readingMode == ReadingMode::Paged) {
        // No scrollbar in paged mode (see applyReadingMode) - both margins,
        // nothing else, reserved.
        const int total = qMax(240, m_content->width() - 2 * 12);
        return m_pagedColumns == 2 ? qMax(240, (total - kPagedColumnGap) / 2) : total;
    }
    return qMax(240, m_content->width() - 2 * 12 - (m_vscroll ? m_vscroll->sizeHint().width() : 0));
}

QString EpubView::cacheRootForBook() const
{
    const QFileInfo info(m_document.path());
    // Keyed by path + size + mtime, not just path: a book replaced on disk
    // (re-downloaded, re-converted) gets a fresh cache directory instead of
    // serving stale extracted images under an unrelated new book.
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(info.absoluteFilePath())
                            .arg(info.size())
                            .arg(info.lastModified().toMSecsSinceEpoch());
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    return appDir(XDGroot::Cache, QStringLiteral("epub/%1").arg(hash));
}

QString EpubView::resourceCachePath(const QString &archivePath) const
{
    return m_cacheRoot + QLatin1String("/res/") + archivePath;
}

QString EpubView::extractToCache(const QString &archivePath) const
{
    // resolveArchivePath already refuses "..", but a book's bytes are written
    // under this path: a second check where the write happens.
    if (archivePath.split(QLatin1Char('/')).contains(QLatin1String("..")) || archivePath.startsWith(QLatin1Char('/')))
        return {};
    const QString cachePath = resourceCachePath(archivePath);
    if (!QFileInfo::exists(cachePath)) {
        const QByteArray bytes = m_document.readEntry(archivePath);
        if (bytes.isEmpty() || !ensurePrivateDir(QFileInfo(cachePath).absolutePath()))
            return {};
        QFile file(cachePath);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(bytes);
    }
    return QFileInfo::exists(cachePath) ? cachePath : QString();
}

QString EpubView::rewriteResourceRefs(QString html, const QString &chapterArchivePath)
{
    static const QRegularExpression attrRe(
        QStringLiteral("\\b(?:src|xlink:href)\\s*=\\s*(\"([^\"]*)\"|'([^']*)')"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    for (auto it = attrRe.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const bool doubleQuoted = match.capturedStart(2) != -1;
        const QString value = doubleQuoted ? match.captured(2) : match.captured(3);
        out += html.mid(last, match.capturedStart() - last);

        QString replacement = value;
        if (!value.isEmpty() && !value.contains(QStringLiteral("://"))
            && !value.startsWith(QLatin1String("data:"))) {
            if (const auto resolved = EpubDocument::resolveArchivePath(chapterArchivePath, value)) {
                const QString cachePath = extractToCache(*resolved);
                if (!cachePath.isEmpty())
                    replacement = cachePath;
                // else: refused (escaped the archive) or unreadable - leave
                // `value` as written, which HtmlRenderer's own filesystem
                // resolution then also fails to find, so nothing draws;
                // same degrade-to-nothing behaviour a missing local path or
                // a remote one already gets (see HtmlRenderer's class
                // comment).
            }
        }
        const QString wholeMatch = match.captured(0);
        const qsizetype eq = wholeMatch.indexOf(QLatin1Char('='));
        const QChar quote = doubleQuoted ? QLatin1Char('"') : QLatin1Char('\'');
        out += wholeMatch.left(eq + 1) + quote + replacement + quote;
        last = match.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

QString EpubView::rewriteCssUrls(const QString &css, const QString &stylesheetArchivePath) const
{
    static const QRegularExpression urlRe(QStringLiteral("url\\(\\s*(['\"]?)([^'\")]+)\\1\\s*\\)"),
                                          QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(css.size());
    qsizetype last = 0;
    for (auto it = urlRe.globalMatch(css); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        out += css.mid(last, match.capturedStart() - last);
        const QString value = match.captured(2);
        QString replacement = value;
        if (!value.isEmpty() && !value.contains(QStringLiteral("://"))
            && !value.startsWith(QLatin1String("data:"))) {
            if (const auto resolved = EpubDocument::resolveArchivePath(stylesheetArchivePath, value)) {
                const QString cachePath = extractToCache(*resolved);
                if (!cachePath.isEmpty())
                    replacement = cachePath;
            }
        }
        out += QStringLiteral("url('%1')").arg(replacement);
        last = match.capturedEnd();
    }
    out += css.mid(last);
    return out;
}

void EpubView::registerChapterFonts(const QString &css, const QString &stylesheetArchivePath)
{
    static const QRegularExpression fontFaceRe(QStringLiteral("@font-face\\s*\\{[^}]*\\}"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression urlRe(QStringLiteral("url\\(\\s*(['\"]?)([^'\")]+)\\1\\s*\\)"),
                                          QRegularExpression::CaseInsensitiveOption);
    for (auto blockIt = fontFaceRe.globalMatch(css); blockIt.hasNext();) {
        const QRegularExpressionMatch urlMatch = urlRe.match(blockIt.next().captured(0));
        if (!urlMatch.hasMatch())
            continue;
        const QString value = urlMatch.captured(2);
        if (value.isEmpty() || value.contains(QStringLiteral("://")) || value.startsWith(QLatin1String("data:")))
            continue; // remote/data: never fetched, same rule as every other resource in this reader
        const auto resolved = EpubDocument::resolveArchivePath(stylesheetArchivePath, value);
        if (!resolved || m_registeredFonts.contains(*resolved))
            continue;
        m_registeredFonts.insert(*resolved); // tried at most once per archive path, success or not
        const QByteArray bytes = m_document.readEntry(*resolved);
        if (!bytes.isEmpty())
            QFontDatabase::addApplicationFontFromData(bytes); // -1 (a format Qt can't load, e.g. WOFF) just leaves the theme's own font in place
    }
}

QString EpubView::collectStylesheets(const QString &html, const QString &chapterArchivePath)
{
    QString css;

    static const QRegularExpression linkRe(
        QStringLiteral("<link\\b[^>]*rel\\s*=\\s*[\"'][^\"']*stylesheet[^\"']*[\"'][^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hrefRe(QStringLiteral("href\\s*=\\s*(\"([^\"]*)\"|'([^']*)')"),
                                           QRegularExpression::CaseInsensitiveOption);
    for (auto it = linkRe.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch hrefMatch = hrefRe.match(it.next().captured(0));
        if (!hrefMatch.hasMatch())
            continue;
        const QString href =
            hrefMatch.capturedStart(2) != -1 ? hrefMatch.captured(2) : hrefMatch.captured(3);
        const auto resolved = EpubDocument::resolveArchivePath(chapterArchivePath, href);
        if (!resolved)
            continue;
        const QByteArray bytes = m_document.readEntry(*resolved);
        if (bytes.isEmpty())
            continue;
        const QString sheetCss = QString::fromUtf8(bytes);
        // Resolved relative to *this stylesheet's own* archive path, per
        // CSS's own rule - not the chapter's, which is what
        // rewriteCssUrls/registerChapterFonts get below for an inline
        // <style> block instead.
        registerChapterFonts(sheetCss, *resolved);
        css += rewriteCssUrls(sheetCss, *resolved) + QLatin1Char('\n');
    }

    // A book that embeds its CSS in a <style> block rather than (or as well
    // as) linking it - common enough in EPUB3 - gets the same treatment,
    // resolved against the chapter's own path (it is physically inside that
    // file).
    static const QRegularExpression styleRe(QStringLiteral("<style\\b[^>]*>(.*?)</style\\s*>"),
                                            QRegularExpression::CaseInsensitiveOption
                                                | QRegularExpression::DotMatchesEverythingOption);
    for (auto it = styleRe.globalMatch(html); it.hasNext();) {
        const QString blockCss = it.next().captured(1);
        registerChapterFonts(blockCss, chapterArchivePath);
        css += rewriteCssUrls(blockCss, chapterArchivePath) + QLatin1Char('\n');
    }

    return css;
}

const EpubView::RenderedChapter *EpubView::renderedChapter(int spineIndex)
{
    if (spineIndex < 0 || spineIndex >= m_document.spine().size())
        return nullptr;

    auto found = m_cache.find(spineIndex);
    if (found != m_cache.end()) {
        m_cacheOrder.removeAll(spineIndex);
        m_cacheOrder.append(spineIndex);
        return &found.value();
    }

    requestChapterRender(spineIndex);
    return nullptr;
}

void EpubView::requestChapterRender(int spineIndex)
{
    if (m_pendingSpine.contains(spineIndex))
        return; // already on its way - see deliverChapterRender

    const QString archivePath = m_document.spine().at(spineIndex).archivePath;
    const QByteArray raw = m_document.readEntry(archivePath);
    if (raw.isEmpty())
        return;

    // Same defence in depth as a note's HTML block (HtmlRenderer::sanitizeHtml),
    // applied to a whole chapter rather than one span.
    const QString sanitized = HtmlRenderer::sanitizeHtml(QString::fromUtf8(raw));
    const QString withImages = rewriteResourceRefs(sanitized, archivePath);
    // Resource extraction and any @font-face registration happen here, on
    // this thread, before dispatch - QFontDatabase::addApplicationFont(FromData)
    // is not safe off the Qt GUI thread (see registerChapterFonts's own
    // comment and PageBody.h's identical rule for MathRenderer), and this is
    // always called from the UI thread (renderedChapter(), from paint/hit-
    // test/prefetch) or from the object's own worker-dispatch below - never
    // from the worker itself.
    const QString css = collectStylesheets(sanitized, archivePath);

    m_pendingSpine.insert(spineIndex);
    const quint64 epoch = m_renderEpoch;
    const HtmlRenderer::Theme theme = rendererTheme();
    const int width = contentWidth();
    const qreal ratio = devicePixelRatioF();

    // The litehtml layout/raster itself - the ~11 ms a chapter costs -
    // runs here, off the UI thread. It only ever paints into a QImage this
    // call owns (never a QWidget/QPainter tied to the window, which is the
    // part Qt requires stay on the GUI thread) and only ever resolves QFonts
    // already sitting in the font database (never registers a new one - see
    // above), which is what makes it safe; see the report for the profiling
    // that confirmed both.
    (void)QtConcurrent::run([this, lifeline = m_lifeline, spineIndex, epoch, withImages, css, theme, width, ratio] {
        QList<HtmlRenderer::TextRun> textRuns;
        QImage image = HtmlRenderer::render(withImages, theme, width, QString(), ratio, css, &textRuns);
        lifeline->post(this, [this, spineIndex, epoch, image = std::move(image), textRuns = std::move(textRuns)]() mutable {
            deliverChapterRender(spineIndex, epoch, std::move(image), std::move(textRuns));
        });
    });
}

void EpubView::deliverChapterRender(int spineIndex, quint64 epoch, QImage image,
                                    QList<HtmlRenderer::TextRun> textRuns)
{
    m_pendingSpine.remove(spineIndex);
    if (epoch != m_renderEpoch)
        return; // superseded by a resize/font-size/theme/paged change since this was requested - see m_renderEpoch

    RenderedChapter chapter;
    chapter.image = std::move(image);
    chapter.textRuns = std::move(textRuns);
    chapter.height = chapter.image.isNull() ? 40 : int(chapter.image.height() / chapter.image.devicePixelRatio());
    chapter.textLength =
        chapter.textRuns.isEmpty() ? 0 : chapter.textRuns.last().offset + chapter.textRuns.last().text.size();

    ensureHeightsSized();
    if (spineIndex < m_chapterHeights.size()) {
        m_chapterHeights[spineIndex] = chapter.height;
        m_chapterHeightExact[spineIndex] = true;
    }

    m_cache.insert(spineIndex, chapter);
    m_cacheOrder.append(spineIndex);
    evictFarChapters(spineIndex);

    // The placeholder painted in its place until now (paintContent/
    // paintPage) is replaced by the real image on this repaint; a taller-
    // or-shorter-than-guessed chapter also needs the scroll range redone.
    updateScrollRange();
    m_content->update();
}

const EpubView::RenderedChapter *EpubView::renderedChapterBlocking(int spineIndex)
{
    if (spineIndex < 0 || spineIndex >= m_document.spine().size())
        return nullptr;

    auto found = m_cache.find(spineIndex);
    if (found != m_cache.end()) {
        m_cacheOrder.removeAll(spineIndex);
        m_cacheOrder.append(spineIndex);
        return &found.value();
    }

    const QString archivePath = m_document.spine().at(spineIndex).archivePath;
    const QByteArray raw = m_document.readEntry(archivePath);
    if (raw.isEmpty())
        return nullptr;

    const QString sanitized = HtmlRenderer::sanitizeHtml(QString::fromUtf8(raw));
    const QString withImages = rewriteResourceRefs(sanitized, archivePath);
    const QString css = collectStylesheets(sanitized, archivePath);

    RenderedChapter chapter;
    const qreal ratio = devicePixelRatioF();
    chapter.image =
        HtmlRenderer::render(withImages, rendererTheme(), contentWidth(), QString(), ratio, css, &chapter.textRuns);
    chapter.height = chapter.image.isNull() ? 40 : int(chapter.image.height() / chapter.image.devicePixelRatio());
    chapter.textLength =
        chapter.textRuns.isEmpty() ? 0 : chapter.textRuns.last().offset + chapter.textRuns.last().text.size();

    ensureHeightsSized();
    if (spineIndex < m_chapterHeights.size()) {
        m_chapterHeights[spineIndex] = chapter.height;
        m_chapterHeightExact[spineIndex] = true;
    }

    // A concurrent worker render of the same chapter may also be in flight
    // (rare - see requestChapterRender) and will overwrite this entry when it
    // lands; harmless, since it renders the identical input to the identical
    // result, just once more than strictly needed.
    auto inserted = m_cache.insert(spineIndex, chapter);
    m_cacheOrder.append(spineIndex);
    evictFarChapters(spineIndex);
    return &inserted.value();
}

void EpubView::evictFarChapters(int aroundSpineIndex)
{
    Q_UNUSED(aroundSpineIndex); // renderedChapter() already keeps m_cacheOrder in LRU order
    while (m_cacheOrder.size() > kMaxCachedChapters) {
        const int victim = m_cacheOrder.takeFirst();
        m_cache.remove(victim);
    }
}

void EpubView::invalidateRenders()
{
    ++m_renderEpoch; // any render already in flight is now for a stale width/theme - deliverChapterRender drops it
    m_cache.clear();
    m_cacheOrder.clear();
    // Heights stay as the best guess so far rather than resetting to the
    // flat estimate - a resize or font change rarely moves a chapter's
    // height by much, so the scrollbar stays close to right until each
    // visible chapter is re-rendered and corrects itself exactly.
    for (int i = 0; i < m_chapterHeightExact.size(); ++i)
        m_chapterHeightExact[i] = false;
}

void EpubView::ensureHeightsSized()
{
    const int n = m_document.spine().size();
    if (m_chapterHeights.size() == n)
        return;
    m_chapterHeights = QList<qint64>(n, 600); // a rough guess, corrected the moment a chapter renders
    m_chapterHeightExact = QList<bool>(n, false);
}

qint64 EpubView::chapterTop(int spineIndex)
{
    ensureHeightsSized();
    qint64 top = 0;
    const int last = qMin(spineIndex, m_chapterHeights.size());
    for (int i = 0; i < last; ++i)
        top += m_chapterHeights.at(i);
    return top;
}

qint64 EpubView::documentHeight()
{
    return chapterTop(m_document.spine().size());
}

int EpubView::chapterAtY(qint64 documentY, qint64 *topOut)
{
    ensureHeightsSized();
    qint64 top = 0;
    for (int i = 0; i < m_chapterHeights.size(); ++i) {
        const qint64 height = m_chapterHeights.at(i);
        if (documentY < top + height || i == m_chapterHeights.size() - 1) {
            if (topOut)
                *topOut = top;
            return i;
        }
        top += height;
    }
    if (topOut)
        *topOut = 0;
    return -1;
}

void EpubView::updateScrollRange()
{
    if (!m_vscroll)
        return;
    const qint64 total = documentHeight();
    const int viewportHeight = qMax(1, m_content->height());
    m_vscroll->setRange(0, int(qMax<qint64>(0, total - viewportHeight)));
    m_vscroll->setPageStep(viewportHeight);
    m_vscroll->setSingleStep(qMax(1, viewportHeight / 10));
}

void EpubView::paintContent(QPainter &painter)
{
    painter.fillRect(m_content->rect(), m_theme.color(QStringLiteral("--background-primary")));
    if (!m_document.isValid()) {
        painter.setPen(m_theme.color(QStringLiteral("--text-muted")));
        painter.drawText(m_content->rect(), Qt::AlignCenter, tr("No book open"));
        return;
    }

    if (m_readingMode == ReadingMode::Paged) {
        paintPaged(painter);
        return;
    }

    qint64 top = 0;
    const int startIndex = chapterAtY(m_vscroll->value(), &top);
    if (startIndex < 0)
        return;

    qint64 y = top - m_vscroll->value();
    int lastPainted = startIndex;
    for (int i = startIndex; i < m_document.spine().size() && y < m_content->height(); ++i) {
        const RenderedChapter *chapter = renderedChapter(i);
        const int height =
            chapter ? chapter->height : int(i < m_chapterHeights.size() ? m_chapterHeights.at(i) : 600);
        if (chapter && !chapter->image.isNull() && y + height > 0)
            painter.drawImage(QPointF(12, y), chapter->image);
        if (chapter)
            paintHighlightsAndSelection(painter, i, y);
        y += height;
        lastPainted = i;
    }

    // The chapter just past the bottom of the screen: renderedChapter()
    // already kicked off (or joined) a worker render for every chapter the
    // loop above actually reached, as a side effect of the cache miss; this
    // reaches one further so scrolling forward at a normal reading pace
    // rarely lands on a chapter whose render has not even started yet. No
    // QTimer::singleShot/QPointer deferral needed for that any more - a
    // cache miss only ever queues a QtConcurrent job now (requestChapterRender),
    // never runs litehtml inline, so calling it straight from paint is cheap.
    const int prefetch = lastPainted + 1;
    if (prefetch < m_document.spine().size())
        renderedChapter(prefetch);
}

void EpubView::paintPaged(QPainter &painter)
{
    const qint64 leftTop = m_vscroll->value();
    paintPage(painter, leftTop, 12);
    if (m_pagedColumns == 2)
        paintPage(painter, leftTop + pageHeight(), 12 + contentWidth() + kPagedColumnGap);

    // Same look-ahead idea as continuous mode's prefetch (paintContent), one
    // page (or spread) further.
    qint64 afterTop = 0;
    const qint64 afterDocY = leftTop + qint64(pageHeight()) * (m_pagedColumns == 2 ? 2 : 1);
    const int afterIndex = chapterAtY(afterDocY, &afterTop);
    if (afterIndex >= 0)
        renderedChapter(afterIndex);
}

void EpubView::paintPage(QPainter &painter, qint64 pageDocTop, int xOffset)
{
    qint64 top = 0;
    const int startIndex = chapterAtY(pageDocTop, &top);
    if (startIndex < 0)
        return;

    const int ph = pageHeight();
    qint64 y = top - pageDocTop;
    for (int i = startIndex; i < m_document.spine().size() && y < ph; ++i) {
        const RenderedChapter *chapter = renderedChapter(i);
        const int height =
            chapter ? chapter->height : int(i < m_chapterHeights.size() ? m_chapterHeights.at(i) : 600);
        // No explicit clip rect: the widget's own paint clip already bounds
        // this call to m_content->rect(), and ph == that rect's height - the
        // same reason continuous mode's single blit above never needed one
        // either. A chapter image taller than one page just has its tail
        // clipped away here exactly like continuous mode's does at the
        // bottom of the viewport.
        if (chapter && !chapter->image.isNull() && y + height > 0)
            painter.drawImage(QPointF(xOffset, y), chapter->image);
        if (chapter)
            paintHighlightsAndSelection(painter, i, y, xOffset);
        y += height;
    }
}

qint64 EpubView::scrollValueForTest() const
{
    return m_vscroll ? m_vscroll->value() : 0;
}

qint64 EpubView::scrollMaximumForTest() const
{
    return m_vscroll ? m_vscroll->maximum() : 0;
}

int EpubView::currentSpineIndexForTest()
{
    return chapterAtY(m_vscroll ? m_vscroll->value() : 0);
}

int EpubView::pageHeight() const
{
    return qMax(1, m_content->height());
}

qint64 EpubView::snappedPageTop(qint64 documentY) const
{
    const qint64 ph = pageHeight();
    return ph > 0 ? (qMax<qint64>(0, documentY) / ph) * ph : documentY;
}

void EpubView::recomputeColumns()
{
    if (m_readingMode != ReadingMode::Paged) {
        m_pagedColumns = 1;
        return;
    }
    const int total = qMax(240, m_content->width() - 2 * 12);
    m_pagedColumns = total >= kTwoColumnMinWidth ? 2 : 1;
}

void EpubView::turnPage(int direction)
{
    if (m_readingMode != ReadingMode::Paged)
        return;
    const qint64 step = qint64(pageHeight()) * (m_pagedColumns == 2 ? 2 : 1);
    const qint64 target = qBound<qint64>(0, m_vscroll->value() + step * direction, qint64(m_vscroll->maximum()));
    m_vscroll->setValue(int(target)); // valueChanged already repaints and debounces savePosition
}

void EpubView::applyReadingMode(bool paged)
{
    m_readingMode = paged ? ReadingMode::Paged : ReadingMode::Continuous;
    recomputeColumns();
    invalidateRenders(); // contentWidth() depends on the mode/column count now - same as a resize
    updateScrollRange();
    if (paged)
        m_vscroll->setValue(int(snappedPageTop(m_vscroll->value())));
    m_vscroll->setVisible(!paged); // paged mode turns pages instead of dragging a bar - see turnPage/margin clicks
    if (m_pagedButton && m_pagedButton->isChecked() != paged)
        m_pagedButton->setChecked(paged); // harmless re-entry into setPagedMode: its own guard no-ops it
    m_content->update();
}

} // namespace noteAppC
