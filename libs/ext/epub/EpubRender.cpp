#include "EpubView.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTimer>

/// Chapters: on-demand litehtml rendering through HtmlRenderer, the image/
/// text-run cache that bounds memory on a big book, resource extraction from
/// the archive, and the document-space layout (cumulative chapter heights)
/// continuous scroll is built on.

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
    const QByteArray xdg = qgetenv("XDG_CACHE_HOME");
    const QString base =
        xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : QString::fromLocal8Bit(xdg);
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(base).filePath(QStringLiteral("noteAppC/epub/%1").arg(hash));
}

QString EpubView::resourceCachePath(const QString &archivePath) const
{
    return m_cacheRoot + QLatin1String("/res/") + archivePath;
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
                const QString cachePath = resourceCachePath(*resolved);
                if (!QFileInfo::exists(cachePath)) {
                    const QByteArray bytes = m_document.readEntry(*resolved);
                    if (!bytes.isEmpty() && QDir().mkpath(QFileInfo(cachePath).absolutePath())) {
                        QFile file(cachePath);
                        if (file.open(QIODevice::WriteOnly))
                            file.write(bytes);
                    }
                }
                if (QFileInfo::exists(cachePath))
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

QString EpubView::collectStylesheets(const QString &html, const QString &chapterArchivePath) const
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
        if (!bytes.isEmpty())
            css += QString::fromUtf8(bytes) + QLatin1Char('\n');
    }

    // A book that embeds its CSS in a <style> block rather than (or as well
    // as) linking it - common enough in EPUB3 - gets the same treatment.
    static const QRegularExpression styleRe(QStringLiteral("<style\\b[^>]*>(.*?)</style\\s*>"),
                                            QRegularExpression::CaseInsensitiveOption
                                                | QRegularExpression::DotMatchesEverythingOption);
    for (auto it = styleRe.globalMatch(html); it.hasNext();)
        css += it.next().captured(1) + QLatin1Char('\n');

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

    const QString archivePath = m_document.spine().at(spineIndex).archivePath;
    const QByteArray raw = m_document.readEntry(archivePath);
    if (raw.isEmpty())
        return nullptr;

    // Same defence in depth as a note's HTML block (HtmlRenderer::sanitizeHtml),
    // applied to a whole chapter rather than one span.
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

    // The chapter just past the bottom of the screen, pre-rendered on an
    // idle callback rather than the paint/scroll path itself, so scrolling
    // forward at a normal reading pace rarely lands on an un-cached
    // chapter's synchronous litehtml layout - see the report on why this
    // stops short of a worker thread.
    const int prefetch = lastPainted + 1;
    if (prefetch < m_document.spine().size() && !m_cache.contains(prefetch)) {
        QPointer<EpubView> self(this);
        QTimer::singleShot(0, this, [self, prefetch] {
            if (self)
                self->renderedChapter(prefetch);
        });
    }
}

} // namespace noteAppC
