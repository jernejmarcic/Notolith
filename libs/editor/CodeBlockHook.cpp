/// A fenced code block in a language a runtime plugin renders (docs/PLUGINS.md,
/// Tier 2). libs/editor does not know plugins exist: the window registers a
/// renderer per language (setCodeBlockRenderer), the same shape as
/// setFileEmbedRenderer, and the block shows what it answers in place of the
/// listing - a placeholder until then, cached by language, width, theme and
/// source. A text answer is drawn as Markdown through renderNoteEmbed; an
/// image is drawn as it is.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

namespace noteAppC {

void MarkdownEditor::setCodeBlockRenderer(const QStringList &languages, CodeBlockRenderer renderer)
{
    for (const QString &language : languages) {
        if (renderer)
            d->m_codeBlockRenderers.insert(language.toLower(), renderer);
        else
            d->m_codeBlockRenderers.remove(language.toLower());
    }
    d->m_codeBlockCache.clear();
    for (BlockView &block : d->m_blocks) {
        if (block.span.kind != BlockSpan::Kind::Code)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
}

bool MarkdownEditor::layoutPluginCode(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code || d->m_codeBlockRenderers.isEmpty())
        return false;
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence)
        return false;
    const QString language = fence->language.toLower();
    const auto rendererIt = d->m_codeBlockRenderers.constFind(language);
    if (rendererIt == d->m_codeBlockRenderers.cend())
        return false;

    const QString source = d->m_text.mid(fence->content.start, fence->content.length());
    const int width = contentWidth();
    const QString key =
        language + QLatin1Char('|') + QString::number(width) + QLatin1Char('|') + embedThemeTag() + QLatin1Char('|') + source;

    auto it = d->m_codeBlockCache.find(key);
    if (it == d->m_codeBlockCache.end()) {
        CodeBlockCacheEntry entry;
        entry.generation = ++d->m_codeBlockGeneration;
        it = d->m_codeBlockCache.insert(key, entry);
        const quint64 generation = entry.generation;
        // Any thread, at most once: only deliverCodeBlock touches the cache
        // or a block, back on this object's thread while the editor lives.
        rendererIt.value()(language, source, width, d->m_theme.isDark() ? QStringLiteral("dark") : QStringLiteral("light"),
                           [this, lifeline = d->m_lifeline, key, generation](CodeBlockResult result) {
                               lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                                   deliverCodeBlock(key, generation, std::move(result));
                               });
                           });
    }
    if (!it->pending && it->image.isNull())
        return false; // nothing drawable came back: the listing shows

    view.isPluginBlock = true;
    view.codeBlockKey = key;
    view.embedLabel = fence->language;
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();
    hideWholeBlockText(view);
    if (it->pending) {
        view.image = QPixmap();
        view.height = view.textTop + 60; // a placeholder until the plugin answers
        view.laidOut = true;
        return true;
    }
    const QPixmap pixmap = QPixmap::fromImage(it->image);
    const qreal ratio = pixmap.devicePixelRatio();
    view.image = pixmap;
    view.imageSize = QSize(qMin(width, int(pixmap.width() / ratio)), int(pixmap.height() / ratio));
    view.imageCentred = false;
    view.height = view.imageSize.height() + view.textTop;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::deliverCodeBlock(const QString &key, quint64 generation, CodeBlockResult result)
{
    auto it = d->m_codeBlockCache.find(key);
    if (it == d->m_codeBlockCache.end() || it->generation != generation)
        return;
    it->pending = false;
    if (result.isMarkdown) {
        const int width = key.section(QLatin1Char('|'), 1, 1).toInt();
        it->image = result.text.isEmpty() ? QImage() : renderNoteEmbed(result.text, width);
    } else {
        it->image = std::move(result.image);
    }

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (block.codeBlockKey != key)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

} // namespace noteAppC
