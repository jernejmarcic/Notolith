#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "MermaidBlock.h"

#include <QPainter>
#include <QPdfWriter>
#include <QScrollBar>


namespace noteAppC {

QList<qreal> MarkdownEditor::pageStarts(const QList<QPair<qreal, qreal>> &blocks, qreal pageHeight)
{
    QList<qreal> starts{0};
    if (pageHeight <= 0)
        return starts;
    for (const auto &[top, height] : blocks) {
        qreal &start = starts.last();
        if (top + height <= start + pageHeight)
            continue; // fits on this page
        if (height <= pageHeight && top > start) {
            starts << top; // the whole block moves to a page of its own
            continue;
        }
        // Taller than a page: it runs on across pages at the page height.
        qreal next = start;
        while (top + height > next + pageHeight)
            starts << (next += pageHeight);
    }
    return starts;
}

bool MarkdownEditor::exportPdf(const QString &path, const QString &text, const QString &title,
                               std::function<QString(const QString &)> resolver,
                               const PdfOptions &options, QString *error)
{
    QPdfWriter writer(path);
    writer.setTitle(title);
    writer.setCreator(QStringLiteral("Notolith"));
    // One unit a pixel at 96 dpi, the density the editor lays out for.
    writer.setResolution(96);
    writer.setPageLayout(QPageLayout(options.pageSize, options.orientation,
                                     QMarginsF(options.marginMillimetres, options.marginMillimetres,
                                               options.marginMillimetres, options.marginMillimetres),
                                     QPageLayout::Millimeter));
    const QRect page = writer.pageLayout().paintRectPixels(writer.resolution());
    const qreal scale = qBound(10, options.downscalePercent, 100) / 100.0;

    const QString printed = text;

    // Paper is white: the light theme, whatever the window uses.
    MarkdownEditor editor;
    editor.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor.setTheme(Theme::builtin(Theme::Mode::Light));
    editor.setReadableLineLength(false);
    editor.setLinkResolver(std::move(resolver));
    editor.setMode(Mode::Reading);
    // A diagram fence (mermaid, plantuml, tikz, dot) is the diagram the editor already made (never the
    // tool: an export waits for nothing); one it never made stays its listing.
    for (const DiagramKind kind : {DiagramKind::Mermaid, DiagramKind::PlantUML, DiagramKind::TikZ, DiagramKind::Graphviz})
        editor.setCodeBlockRenderer(diagramLanguages(kind), diagramCachedRenderer(kind, Theme::builtin(Theme::Mode::Light)));
    const QSize viewport(int(page.width() / scale), int(page.height() / scale));
    editor.resize(viewport);
    editor.setNoteText(printed);
    editor.finishParse(); // a big note parses on the pool

    // Everything laid out, not just what a screen would show.
    for (BlockView &view : editor.d->m_blocks) {
        if (!view.laidOut)
            editor.layoutBlock(view);
    }
    QCoreApplication::processEvents(); // the cached diagrams the layout asked for land, and re-lay-out their blocks
    for (BlockView &view : editor.d->m_blocks) {
        if (!view.laidOut)
            editor.layoutBlock(view);
    }
    editor.updateLayout();

    // Obsidian puts the file name above everything, the properties too - so
    // it is drawn here rather than written into the text, where it would sit
    // in front of the frontmatter and stop it being frontmatter.
    const bool withTitle = options.includeTitle && !title.isEmpty();
    QFont titleFont = editor.d->m_theme.textFont();
    titleFont.setPixelSize(int(titleFont.pixelSize() * 2.0));
    titleFont.setWeight(QFont::Bold);
    const qreal titleHeight = withTitle ? QFontMetricsF(titleFont).height() * 1.8 : 0;

    // Laid out as if the title were a block of its own at the top.
    QList<QPair<qreal, qreal>> extents;
    for (const BlockView &view : editor.d->m_blocks) {
        if (!view.tabHidden && view.height > 0)
            extents.append({view.y + titleHeight, view.height});
    }
    const qreal pageHeight = page.height() / scale;
    const QList<qreal> starts = pageStarts(extents, pageHeight);

    QPainter painter;
    if (!painter.begin(&writer)) {
        if (error)
            *error = tr("Could not write %1").arg(path);
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    for (qsizetype i = 0; i < starts.size(); ++i) {
        if (i > 0)
            writer.newPage();
        const qreal start = starts.at(i);
        const qreal end = i + 1 < starts.size() ? starts.at(i + 1) : start + pageHeight;
        painter.save();
        painter.scale(scale, scale);
        // Only this page's share: the next page starts where this one stops.
        painter.setClipRect(QRectF(0, 0, viewport.width(), end - start));
        if (i == 0 && withTitle) {
            painter.setFont(titleFont);
            painter.setPen(editor.d->m_theme.color(QStringLiteral("--text-normal")));
            painter.drawText(QRectF(editor.contentLeft(), 0, editor.contentWidth(), titleHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, title);
        }
        editor.paintBlocks(painter, start - titleHeight, 0, end - start);
        painter.restore();
    }
    return painter.end();
}

} // namespace noteAppC
