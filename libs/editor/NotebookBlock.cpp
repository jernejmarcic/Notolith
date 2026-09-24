/// A fenced code block's Jupyter-kernel output (stage 13/docs/PLUGINS.md,
/// Tier 1): unlike CodeBlockHook.cpp's plugin fences, this never replaces
/// the listing - the source stays exactly as written, a run control joins
/// "Copy" in the header (codeLabelOf, paintCodeChrome, handleBlockClick),
/// and any output from the block's last run trails below it, the same
/// "append after the block's own content" idea appendEmbedRows already
/// uses for an embed a paragraph could not show inline.
///
/// Everything here costs nothing until a block with a language is actually
/// laid out (view.notebookEligible, decided once in layoutBlock) - and even
/// then, only a cache lookup happens (forceRun=false, never starts a
/// kernel) until the reader clicks the run control or the window's own
/// "Run all blocks" command calls runNotebookBlockAt for them. libs/editor
/// never links libs/ext/notebook (the module boundary docs/PLUGINS.md
/// sets out): m_notebookRunner is supplied by the window, the same shape
/// setQueryRunner and setCodeBlockRenderer already use.

#include "MarkdownEditor.h"

#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QTextLayout>

#include <cmath>

namespace noteAppC {

namespace {

/// BlockRenderers.cpp's own kFoldHandle, which is file-local there: the
/// side of the square a fold arrow occupies, and what the run control's
/// own rect leaves clear so the two never overlap in a `#| fold`able block.
constexpr qreal kFoldHandle = 14;

/// A second, cheap pass over stream/error text for a stray ANSI escape a
/// runner did not already strip (libs/ext/notebook's own NotebookController
/// does, but this editor must not assume every caller does) - never shown
/// as literal control bytes in place of colour it cannot have anyway.
QString stripAnsi(const QString &text)
{
    static const QRegularExpression escape(QStringLiteral("\x1b\\[[0-9;]*m"));
    QString out = text;
    out.remove(escape);
    return out;
}

} // namespace

void MarkdownEditor::setNotebookRunner(NotebookRunner runner)
{
    m_notebookRunner = std::move(runner);
    m_notebookCache.clear();
    bool touched = false;
    for (BlockView &block : m_blocks) {
        // Gated on the block *kind*, not the old block.notebookEligible -
        // that flag was decided under the previous runner (null, the first
        // time this is ever called) and every Code block needs a chance to
        // become eligible under the new one, the same reasoning
        // setCodeBlockRenderer's own reset loop follows. A runnable
        // `![[script.py]]` is a source-file embed, which a paragraph holds.
        if (block.span.kind != BlockSpan::Kind::Code && !block.embedPanel && !block.notebookEligible)
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

const MarkdownEditor::NotebookCacheEntry &MarkdownEditor::ensureNotebookResult(
    const QString &key, const QString &language, const QString &source, const QString &workingDir,
    bool sandboxHost)
{
    auto it = m_notebookCache.find(key);
    if (it != m_notebookCache.end())
        return *it;

    NotebookCacheEntry entry;
    entry.pending = true;
    entry.generation = ++m_notebookGeneration;
    it = m_notebookCache.insert(key, entry);

    if (m_notebookRunner) {
        const quint64 generation = entry.generation;
        NotebookRunOptions options;
        options.sandboxHost = sandboxHost;
        options.workingDir = workingDir;
        // forceRun=false: a cache-only lookup. Opening a note must never
        // start a kernel by itself - see the module header comment above.
        m_notebookRunner(language, source, false, options,
                         [this, lifeline = m_lifeline, key, generation](NotebookRunResult result) {
                             lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                                 deliverNotebookResult(key, generation, std::move(result));
                             });
                         });
    }
    return *it;
}

void MarkdownEditor::deliverNotebookResult(const QString &key, quint64 generation, NotebookRunResult result)
{
    auto it = m_notebookCache.find(key);
    if (it == m_notebookCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->running = false;
    it->result = std::move(result);
    // Invalidated, not rebuilt here: renderNotebookImage runs off
    // appendNotebookOutput's own contentWidth(), which this delivery
    // callback (running on any thread originally, marshalled here) has no
    // business assuming still matches.
    it->renderedImages.clear();
    it->renderedWidth = 0;

    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (block.notebookKey != key)
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

std::unique_ptr<QTextLayout> MarkdownEditor::buildNotebookTextLayout(const QString &text, qreal width,
                                                                     const QColor &colour) const
{
    if (text.isEmpty())
        return nullptr;
    auto layout = std::make_unique<QTextLayout>(text);
    layout->setCacheEnabled(true);
    layout->setFont(fontFor({BlockSpan::Kind::Code, {0, 0}, 0}));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout->setTextOption(option);
    QTextLayout::FormatRange format;
    format.start = 0;
    format.length = int(text.size());
    format.format.setForeground(colour);
    layout->setFormats({format});

    const qreal lineHeight = lineHeightFor({BlockSpan::Kind::Code, {0, 0}, 0});
    layout->beginLayout();
    qreal y = 0;
    while (true) {
        QTextLine line = layout->createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        line.setPosition(QPointF(0, y));
        y += lineHeight;
    }
    layout->endLayout();
    return layout;
}

QPixmap MarkdownEditor::renderNotebookImage(const NotebookOutput &output, int width) const
{
    switch (output.kind) {
    case NotebookOutput::Kind::Markdown: {
        if (output.text.trimmed().isEmpty())
            return {};
        return QPixmap::fromImage(renderNoteEmbed(output.text, width));
    }
    case NotebookOutput::Kind::ImagePng: {
        QImage image;
        if (!image.loadFromData(output.imageBytes, "PNG"))
            return {};
        return QPixmap::fromImage(image);
    }
    case NotebookOutput::Kind::ImageSvg: {
        const QByteArray svg = output.text.toUtf8();
        QSvgRenderer renderer(svg);
        if (!renderer.isValid())
            return {};
        QSizeF natural = renderer.viewBoxF().size();
        if (natural.isEmpty())
            natural = QSizeF(width, width * 0.6);
        const qreal w = qMin<qreal>(width, natural.width());
        const qreal h = w * natural.height() / qMax<qreal>(1.0, natural.width());
        QImage image(QSize(qCeil(w), qCeil(h)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0, 0, w, h));
        painter.end();
        return QPixmap::fromImage(image);
    }
    case NotebookOutput::Kind::Text:
    case NotebookOutput::Kind::Error:
        return {}; // text kinds never reach here - see appendNotebookOutput
    }
    return {};
}

void MarkdownEditor::appendNotebookMessage(BlockView &view, const QString &text, const QString &colourToken)
{
    if (text.isEmpty())
        return;
    BlockView::NotebookOutputCell cell;
    cell.kind = NotebookOutput::Kind::Text;
    const qreal width = contentWidth() - 28;
    cell.layout = buildNotebookTextLayout(text, width, m_theme.color(colourToken));
    if (!cell.layout)
        return;
    const qreal height = cell.layout->boundingRect().height();
    cell.rect = QRectF(0, view.height + 10, width, height);
    view.height = cell.rect.bottom() + 8;
    view.notebookOutputs.push_back(std::move(cell));
}

void MarkdownEditor::appendNotebookOutput(BlockView &view, const QString &language, const QString &source,
                                          const QString &workingDir, bool sandboxHost)
{
    // The tier is part of the key: a host-tier and a bubblewrap-tier run of
    // the same source must never share a cached result (this stage's own
    // requirement) - the working directory is not, since it never changes
    // what the code itself does, only what files sit beside it.
    const QString key = language.trimmed().toLower() + QLatin1Char('|')
                       + (sandboxHost ? QStringLiteral("host") : QStringLiteral("bwrap"))
                       + QLatin1Char('|') + source;
    view.notebookKey = key;
    view.notebookLanguage = language;
    view.notebookSource = source;
    view.notebookWorkingDir = workingDir;
    view.notebookSandboxHost = sandboxHost;
    view.notebookOutputs.clear();
    view.notebookOutputTop = view.height;

    const NotebookCacheEntry &lookup = ensureNotebookResult(key, language, source, workingDir, sandboxHost);
    view.notebookHasRun = lookup.result.hasRun;
    view.notebookRunning = lookup.running;
    view.notebookOk = lookup.result.ok && lookup.result.kernelError.isEmpty();

    if (lookup.running) {
        appendNotebookMessage(view, tr("Running..."), QStringLiteral("--text-muted"));
        return;
    }
    if (!lookup.result.kernelError.isEmpty()) {
        appendNotebookMessage(view, lookup.result.kernelError, QStringLiteral("--text-error"));
        return;
    }
    if (!lookup.result.hasRun || lookup.result.outputs.isEmpty())
        return; // never run, or ran clean with nothing to show: no output area at all

    // Mutable from here: rendering an image is a cache fill, not a fresh
    // answer, so it belongs to this layout pass rather than to whatever
    // thread delivered the result.
    auto it = m_notebookCache.find(key);
    if (it == m_notebookCache.end())
        return; // cannot happen (ensureNotebookResult above just inserted or found it)
    NotebookCacheEntry &entry = *it;

    const int width = int(contentWidth() - 28);
    const bool needsRender = entry.renderedWidth != width
                             || entry.renderedImages.size() != entry.result.outputs.size();
    if (needsRender) {
        entry.renderedImages.clear();
        for (const NotebookOutput &output : entry.result.outputs)
            entry.renderedImages.append(renderNotebookImage(output, width));
        entry.renderedWidth = width;
    }

    qreal y = view.height + 10;
    for (int i = 0; i < entry.result.outputs.size(); ++i) {
        const NotebookOutput &output = entry.result.outputs.at(i);
        BlockView::NotebookOutputCell cell;
        cell.kind = output.kind;
        qreal cellWidth = width;
        qreal cellHeight = 0;

        if (output.kind == NotebookOutput::Kind::Text) {
            cell.layout = buildNotebookTextLayout(stripAnsi(output.text), width,
                                                  m_theme.color(QStringLiteral("--text-normal")));
        } else if (output.kind == NotebookOutput::Kind::Error) {
            QString text = output.ename;
            if (!output.text.isEmpty())
                text += (text.isEmpty() ? QString() : QStringLiteral(": ")) + output.text;
            if (!output.traceback.isEmpty()) {
                if (!text.isEmpty())
                    text += QLatin1Char('\n');
                text += stripAnsi(output.traceback.join(QLatin1Char('\n')));
            }
            cell.layout = buildNotebookTextLayout(text, width, m_theme.color(QStringLiteral("--text-error")));
        } else {
            cell.image = i < entry.renderedImages.size() ? entry.renderedImages.at(i) : QPixmap();
        }

        if (cell.layout) {
            cellHeight = cell.layout->boundingRect().height();
        } else if (!cell.image.isNull()) {
            const qreal ratio = cell.image.devicePixelRatio();
            const qreal naturalWidth = cell.image.width() / qMax<qreal>(1.0, ratio);
            cellWidth = qMin<qreal>(width, naturalWidth);
            cellHeight = cellWidth * (cell.image.height() / ratio) / qMax<qreal>(1.0, naturalWidth);
        }
        if (cellHeight <= 0)
            continue; // an empty text output or an image that failed to decode: no gap for it

        cell.rect = QRectF(0, y, cellWidth, cellHeight);
        y += cellHeight + 8;
        view.notebookOutputs.push_back(std::move(cell));
    }
    view.height = y;
}

// --- the run control, in the header strip alongside "Copy" ----------------

QRectF MarkdownEditor::notebookRunRectFor(const BlockView &view, qreal left, qreal top) const
{
    if (!view.notebookEligible || view.piece > 0)
        return {};
    const QFontMetricsF metrics(m_theme.uiFont());
    const qreal size = metrics.height();
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    // The header row every notebook-eligible block gets (view.textTop was
    // set to codeHeaderHeight() for exactly this reason - see layoutBlock).
    const qreal y = panel.top() + (view.textTop + 8 - size) / 2;
    // Left of the language label codeLabelOf draws at the header's right
    // end, never on it: placed by the label's own width, since a fixed
    // offset put the icon over the start of "haskell".
    const CodeLabel label = codeLabelOf(view, left, top);
    const qreal right = label.rect.isValid() ? label.rect.left() - 8 - size : panel.right() - 22 - kFoldHandle - size;
    return QRectF(right, y, size, size);
}

void MarkdownEditor::paintNotebookRunControl(QPainter &painter, const BlockView &view, qreal left,
                                             qreal top) const
{
    const QRectF rect = notebookRunRectFor(view, left, top);
    if (!rect.isValid())
        return;
    painter.save();
    const QColor colour = view.notebookRunning ? m_theme.color(QStringLiteral("--text-muted"))
                          : !view.notebookOk    ? m_theme.color(QStringLiteral("--text-error"))
                                                : m_theme.color(QStringLiteral("--text-muted"));
    // "square" (running - nothing here interrupts it; that is a window-level
    // command, not a per-block click), "triangle-alert" (last run failed) or
    // "play" (idle, including "never run yet"). Icons::icon falls back to
    // nothing drawn for a name the icon set does not have, so an icon-less
    // theme still leaves the rect clickable, just blank.
    const QString name = view.notebookRunning ? QStringLiteral("square")
                         : !view.notebookOk    ? QStringLiteral("triangle-alert")
                                               : QStringLiteral("play");
    const QIcon icon = Icons::icon(name, colour, int(rect.height()));
    if (!icon.isNull())
        icon.paint(&painter, rect.toRect());
    painter.restore();
}

void MarkdownEditor::paintNotebookHostBadge(QPainter &painter, const BlockView &view, qreal left,
                                            qreal top) const
{
    if (!view.notebookSandboxHost)
        return;
    const QRectF run = notebookRunRectFor(view, left, top);
    if (!run.isValid())
        return;
    QFont font = m_theme.uiFont();
    font.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    const QFontMetricsF metrics(font);
    const QString text = tr("host");
    const qreal width = metrics.horizontalAdvance(text);
    const QRectF rect(run.left() - width - 8, run.top(), width, run.height());

    painter.save();
    painter.setFont(font);
    painter.setPen(m_theme.color(QStringLiteral("--text-muted")));
    painter.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.restore();
}

void MarkdownEditor::paintNotebookOutput(QPainter &painter, const BlockView &view, qreal left,
                                         qreal top) const
{
    if (view.notebookOutputs.empty())
        return;
    painter.save();
    for (const BlockView::NotebookOutputCell &cell : view.notebookOutputs) {
        const QRectF rect = cell.rect.translated(left, top);
        if (cell.layout) {
            cell.layout->draw(&painter, rect.topLeft());
        } else if (!cell.image.isNull()) {
            painter.drawPixmap(rect.toRect(), cell.image);
        }
    }
    painter.restore();
}

// --- the window's own commands ("Run all blocks", the tests) --------------

MarkdownEditor::NotebookBlockInfo MarkdownEditor::notebookInfoAt(qsizetype offset) const
{
    NotebookBlockInfo info;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return info;
    const BlockView &view = m_blocks[size_t(index)];
    if (!view.notebookEligible)
        return info;
    info.eligible = true;
    info.hasRun = view.notebookHasRun;
    info.running = view.notebookRunning;
    info.ok = view.notebookOk;
    const qreal left = contentLeft();
    const qreal top = view.y - verticalScrollBar()->value();
    info.runRect = notebookRunRectFor(view, left, top);
    info.labelRect = codeLabelOf(view, left, top).rect;
    return info;
}

int MarkdownEditor::runAllNotebookBlocks()
{
    int count = 0;
    for (const CodeFenceSpan &fence : std::as_const(m_decorations.codeFences))
        count += runNotebookBlockAt(fence.content.start) ? 1 : 0;
    return count;
}

QString MarkdownEditor::codeLanguageAt(qsizetype offset) const
{
    for (const CodeFenceSpan &fence : m_decorations.codeFences) {
        if (fence.block.start <= offset && offset < fence.block.end)
            return fence.language.trimmed().toLower();
    }
    return {};
}

bool MarkdownEditor::runNotebookBlockAt(qsizetype offset)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return false;
    BlockView &view = m_blocks[size_t(index)];
    if (!view.notebookEligible || !m_notebookRunner)
        return false;

    // Exactly what appendNotebookOutput last laid this block out with -
    // a fenced block's own fence/content, or a runnable embed's file - so a
    // click always runs what is actually on screen, from the one place that
    // ever derives it, instead of a second re-derivation here going stale
    // against the first.
    const QString language = view.notebookLanguage;
    const QString source = view.notebookSource;
    const QString key = view.notebookKey;
    NotebookRunOptions options;
    options.sandboxHost = view.notebookSandboxHost;
    options.workingDir = view.notebookWorkingDir;

    NotebookCacheEntry &entry = m_notebookCache[key]; // inserted fresh if this is the first run
    if (entry.running)
        return true; // already in flight - a second click does nothing new
    entry.running = true;
    entry.pending = false;
    const quint64 generation = ++m_notebookGeneration;
    entry.generation = generation;

    view.notebookRunning = true;
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();

    m_notebookRunner(language, source, /*forceRun=*/true, options,
                     [this, lifeline = m_lifeline, key, generation](NotebookRunResult result) {
                         lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                             deliverNotebookResult(key, generation, std::move(result));
                         });
                     });
    return true;
}

} // namespace noteAppC
