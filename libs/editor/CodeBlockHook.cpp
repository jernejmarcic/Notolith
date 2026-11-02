// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
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

void MarkdownEditor::setProcessGate(ProcessGate gate)
{
    d->m_processGate = std::move(gate);
    for (BlockView &block : d->m_blocks) {
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
}

QString MarkdownEditor::codeBlockKeyFor(const QString &language, const QString &source, int width) const
{
    return language + QLatin1Char('|') + QString::number(width) + QLatin1Char('|') + embedThemeTag() + QLatin1Char('|') + source;
}

int MarkdownEditor::texCompilesForTest() const
{
    return int(d->m_texCache.size());
}

int MarkdownEditor::codeBlockCacheSizeForTest() const
{
    return int(d->m_codeBlockCache.size());
}

void MarkdownEditor::pruneCodeBlockCache(const QString &keep)
{
    constexpr qsizetype kCap = 64;
    if (d->m_codeBlockCache.size() <= kCap)
        return;
    QSet<QString> shown;
    for (const BlockView &block : d->m_blocks)
        if (block.extras) // most blocks have none: no static-default guard per block
            shown.insert(block.extras->codeBlockKey);
    for (auto it = d->m_codeBlockCache.begin(); it != d->m_codeBlockCache.end();) {
        if (it.key() != keep && !shown.contains(it.key()))
            it = d->m_codeBlockCache.erase(it);
        else
            ++it;
    }
}

const MarkdownEditor::CodeBlockCacheEntry *MarkdownEditor::requestCodeBlock(const QString &language,
                                                                             const QString &source, int width,
                                                                             QString *key)
{
    const auto rendererIt = d->m_codeBlockRenderers.constFind(language);
    if (rendererIt == d->m_codeBlockRenderers.cend())
        return nullptr;
    *key = codeBlockKeyFor(language, source, width);

    auto it = d->m_codeBlockCache.find(*key);
    if (it == d->m_codeBlockCache.end()) {
        CodeBlockCacheEntry entry;
        entry.generation = ++d->m_codeBlockGeneration;
        pruneCodeBlockCache(*key);
        it = d->m_codeBlockCache.insert(*key, entry);
        const quint64 generation = entry.generation;
        // Any thread, at most once: only deliverCodeBlock touches the cache
        // or a block, back on this object's thread while the editor lives.
        rendererIt.value()(language, source, width, d->m_theme.isDark() ? QStringLiteral("dark") : QStringLiteral("light"),
                           [this, lifeline = d->m_lifeline, key = *key, generation](CodeBlockResult result) {
                               lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                                   deliverCodeBlock(key, generation, std::move(result));
                               });
                           });
    }
    return &it.value();
}

bool MarkdownEditor::layoutPluginCode(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code || d->m_codeBlockRenderers.isEmpty())
        return false;
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence)
        return false;
    const QString language = fence->language.toLower();
    if (!d->m_codeBlockRenderers.contains(language))
        return false;

    const QString source = d->m_text.mid(fence->content.start, fence->content.length());
    const int width = contentWidth();
    QString key;
    const CodeBlockCacheEntry *entry = requestCodeBlock(language, source, width, &key);
    if (!entry->pending && entry->image.isNull())
        return false; // nothing drawable came back: the listing shows

    view.isPluginBlock = true;
    view.extrasMut().codeBlockKey = key;
    // A long fence is in pieces, each laid out as a block: the first one is the drawing and the rest
    // are nothing, or the drawing would be repeated once per 32 lines of source.
    if (view.piece > 0) {
        view.extrasMut().image = QPixmap();
        view.extrasMut().embedPanel = false;
        view.layout.reset();
        view.map = DisplayMap();
        view.height = 0;
        view.laidOut = true;
        return true;
    }
    view.extrasMut().embedLabel = fence->language;
    view.extrasMut().embedPanel = true;
    view.layout.reset();
    view.extrasMut().cells.clear();
    view.textTop = codeHeaderHeight();
    hideWholeBlockText(view);
    if (entry->pending) {
        view.extrasMut().image = QPixmap();
        view.height = view.textTop + 60; // a placeholder until the plugin answers
        view.laidOut = true;
        return true;
    }
    const QPixmap pixmap = QPixmap::fromImage(entry->image);
    const qreal ratio = pixmap.devicePixelRatio();
    view.extrasMut().image = pixmap;
    view.extrasMut().imageSize = QSize(qMin(width, int(pixmap.width() / ratio)), int(pixmap.height() / ratio));
    view.extrasMut().imageCentred = false;
    view.height = view.x().imageSize.height() + view.textTop;
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
    it->hotspots = result.isMarkdown ? QList<CodeBlockResult::Hotspot>() : std::move(result.hotspots);
    it->activate = result.isMarkdown ? nullptr : std::move(result.activate);

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || block.extras->codeBlockKey != key)
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

QRect MarkdownEditor::codeBlockImageRectAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    return view.isPluginBlock && !view.x().image.isNull() ? hookedEmbedImageRect(view).toRect() : QRect();
}

bool MarkdownEditor::pressCodeBlockHotspot(const QPoint &viewportPoint)
{
    for (const BlockView &view : d->m_blocks) {
        if (!view.isPluginBlock || view.x().image.isNull())
            continue;
        const auto it = d->m_codeBlockCache.constFind(view.x().codeBlockKey);
        if (it == d->m_codeBlockCache.cend() || it->hotspots.isEmpty() || !it->activate)
            continue;
        const QRectF image = hookedEmbedImageRect(view);
        if (!image.contains(viewportPoint))
            continue;
        const QPointF local = viewportPoint - image.topLeft();
        for (const CodeBlockResult::Hotspot &hotspot : it->hotspots) {
            if (!hotspot.rect.contains(local))
                continue;
            // The picture it leads to comes the way any answer does; until then the old one stays, and an
            // answer that is overtaken by another click is dropped (the generation).
            const QString key = view.x().codeBlockKey;
            const int id = hotspot.id;
            const quint64 generation = ++d->m_codeBlockGeneration;
            auto activate = it->activate;
            d->m_codeBlockCache[key].generation = generation;
            activate(id, [this, lifeline = d->m_lifeline, key, generation](CodeBlockResult result) {
                lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                    deliverCodeBlock(key, generation, std::move(result));
                });
            });
            return true;
        }
    }
    return false;
}

} // namespace noteAppC
