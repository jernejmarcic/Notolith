// SPDX-License-Identifier: GPL-3.0-or-later
/// A fenced code block's Jupyter-kernel output (docs/PLUGINS.md,
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
#include "MarkdownEditor_p.h"

#include "HtmlRenderer.h"
#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSvgRenderer>
#include "SafeSvg.h"
#include "ImageLoad.h"
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
    d->m_notebookRunner = std::move(runner);
    d->m_notebookCache.clear();
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        // Gated on the block *kind*, not the old block.notebookEligible -
        // that flag was decided under the previous runner (null, the first
        // time this is ever called) and every Code block needs a chance to
        // become eligible under the new one, the same reasoning
        // setCodeBlockRenderer's own reset loop follows. A runnable
        // `![[script.py]]` is a source-file embed, which a paragraph holds.
        if (block.span.kind != BlockSpan::Kind::Code && !(block.extras && block.extras->embedPanel) && !block.notebookEligible)
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

void MarkdownEditor::setNotebookRunnable(NotebookRunnable runnable)
{
    d->m_notebookRunnable = std::move(runnable);
    setNotebookRunner(d->m_notebookRunner); // every block decides again under the new answer
}

const MarkdownEditor::NotebookCacheEntry &MarkdownEditor::ensureNotebookResult(
    const QString &key, const QString &language, const QString &source, const NotebookRunOptions &options)
{
    auto it = d->m_notebookCache.find(key);
    if (it != d->m_notebookCache.end())
        return *it;

    NotebookCacheEntry entry;
    entry.pending = true;
    entry.generation = ++d->m_notebookGeneration;
    it = d->m_notebookCache.insert(key, entry);

    if (d->m_notebookRunner) {
        const quint64 generation = entry.generation;
        // forceRun=false: a cache-only lookup. Opening a note must never
        // start a kernel by itself - see the module header comment above.
        d->m_notebookRunner(language, source, false, options,
                         [this, lifeline = d->m_lifeline, key, generation](NotebookRunResult result) {
                             lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                                 deliverNotebookResult(key, generation, std::move(result));
                             });
                         });
    }
    return *it;
}

void MarkdownEditor::deliverNotebookResult(const QString &key, quint64 generation, NotebookRunResult result)
{
    auto it = d->m_notebookCache.find(key);
    if (it == d->m_notebookCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->running = false;
    dropNotebookInput(key); // the run is over: nobody is waiting for a line any more
    it->result = std::move(result);
    // Invalidated, not rebuilt here: renderNotebookImage runs off
    // appendNotebookOutput's own contentWidth(), which this delivery
    // callback (running on any thread originally, marshalled here) has no
    // business assuming still matches.
    it->renderedImages.clear();
    it->renderedWidth = 0;

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || block.extras->notebookKey != key)
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
    // QTextLayout breaks a line only at U+2028, never at \n: without this
    // every output is one run-on line.
    QString shown = text;
    shown.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));
    auto layout = std::make_unique<QTextLayout>(shown);
    layout->setCacheEnabled(true);
    layout->setFont(fontFor({BlockSpan::Kind::Code, {0, 0}, 0}));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout->setTextOption(option);
    QTextLayout::FormatRange format;
    format.start = 0;
    format.length = int(shown.size());
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
        // PNG or JPEG: the data says which (and an SVG sent as a picture is cleaned like any other)
        const QImage image = decodeImage(output.imageBytes);
        if (image.isNull())
            return {};
        return QPixmap::fromImage(image);
    }
    case NotebookOutput::Kind::ImageSvg: {
        const QByteArray svg = output.text.toUtf8();
        QSvgRenderer renderer(safeSvg(svg));
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
    case NotebookOutput::Kind::Html: {
        // The same litehtml the note's own HTML blocks use: scripts stripped,
        // nothing fetched. Null without it (or for markup with nothing to
        // see), and appendNotebookOutput then falls back to the plain text.
        if (!d->m_features.HTMLblocks || !HtmlRenderer::isAvailable() || output.text.trimmed().isEmpty())
            return {};
        HtmlRenderer::Theme theme;
        theme.text = d->m_theme.color(QStringLiteral("--text-normal"));
        theme.background = d->m_theme.color(QStringLiteral("--background-primary"));
        theme.link = d->m_theme.color(QStringLiteral("--text-accent"));
        theme.border = d->m_theme.color(QStringLiteral("--background-modifier-border"));
        theme.codeBackground = d->m_theme.color(QStringLiteral("--background-primary-alt"));
        theme.textFont = d->m_theme.textFont();
        theme.codeFont = d->m_theme.monospaceFont();
        const qreal ratio = devicePixelRatioF();
        // A kernel's HTML output names no file of the vault's: no local image at all.
        const QImage image = HtmlRenderer::render(output.text, theme, width, QString(), ratio, QString(), nullptr,
                                                  [](const QString &) { return QString(); });
        return image.isNull() ? QPixmap() : QPixmap::fromImage(image);
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
    BlockExtras::NotebookOutputCell cell;
    cell.kind = NotebookOutput::Kind::Text;
    const qreal width = contentWidth() - 28;
    cell.layout = buildNotebookTextLayout(text, width, d->m_theme.color(colourToken));
    if (!cell.layout)
        return;
    const qreal height = cell.layout->boundingRect().height();
    cell.rect = QRectF(0, view.height + 10, width, height);
    view.height = cell.rect.bottom() + 8;
    view.extrasMut().notebookOutputs.push_back(std::move(cell));
}

MarkdownEditor::NotebookRunOptions MarkdownEditor::notebookOptionsOf(const BlockView &view) const
{
    NotebookRunOptions options;
    options.sandboxWasm = view.x().notebookSandboxWasm;
    options.workingDir = view.x().notebookWorkingDir;
    options.cache = view.x().notebookCacheOn;
    options.timeoutSeconds = view.x().notebookTimeout;
    options.kernel = view.x().notebookKernel;
    options.implicit = view.x().notebookImplicit;
    options.implicitMain = view.x().notebookImplicitMain;
    options.duplicateOrdinal = view.x().notebookOrdinal;
    return options;
}

void MarkdownEditor::appendNotebookOutput(BlockView &view, const QString &language, const QString &source,
                                          const QString &blockID, const QString &workingDir,
                                          const CodeOptions *options, int duplicateOrdinal)
{
    appendNotebookOutputRows(view, language, source, blockID, workingDir, options, duplicateOrdinal);
    appendNotebookInput(view);
}

void MarkdownEditor::appendNotebookOutputRows(BlockView &view, const QString &language, const QString &source,
                                              const QString &blockID, const QString &workingDir,
                                              const CodeOptions *options, int duplicateOrdinal)
{
    const bool sandboxWasm = options && options->sandboxWasm;
    // The tier is part of the key: a wasm-tier and a bubblewrap-tier run of
    // the same source must never share a cached result - the working directory is not, since it never changes
    // what the code itself does, only what files sit beside it. The kernel
    // choice is: it changes what state the code runs against.
    QString key = language.trimmed().toLower() + QLatin1Char('|')
                  + (sandboxWasm ? QStringLiteral("wasm") : QStringLiteral("bwrap"))
                  + QLatin1Char('|') + (options ? options->kernel : QString()) + QLatin1Char('|') + source;
    // The note's own `implicit-<lang>[-main]` frontmatter, unless the block
    // opts out. Part of the key: editing it makes the block look itself up
    // again and its old output Outdated. Limit: `py` is not `python`, a
    // note using both spellings sets both keys.
    view.extrasMut().notebookImplicit.clear();
    view.extrasMut().notebookImplicitMain.clear();
    if (!options || options->implicit) {
        const QString prefix = QStringLiteral("implicit-") + language.trimmed().toLower();
        for (const Property &property : std::as_const(d->m_properties)) {
            const QString name = property.key.toLower();
            if (name == prefix)
                view.extrasMut().notebookImplicit = property.values.value(0);
            else if (name == prefix + QLatin1String("-main"))
                view.extrasMut().notebookImplicitMain = property.values.value(0);
        }
    }
    if (!view.x().notebookImplicit.isEmpty() || !view.x().notebookImplicitMain.isEmpty())
        key += QLatin1Char('\x1f') + view.x().notebookImplicit + QLatin1Char('\x1f') + view.x().notebookImplicitMain;
    // Two blocks that read the same are two results (a notebook's identical cells); the first keeps the key it had.
    if (duplicateOrdinal > 0)
        key += QLatin1Char('\x1e') + QString::number(duplicateOrdinal);
    view.extrasMut().notebookOrdinal = duplicateOrdinal;
    view.extrasMut().notebookKey = key;
    view.extrasMut().notebookLanguage = language;
    view.extrasMut().notebookSource = source;
    view.extrasMut().notebookWorkingDir = workingDir;
    view.extrasMut().notebookSandboxWasm = sandboxWasm;
    view.extrasMut().notebookCacheOn = !options || options->cache;
    view.extrasMut().notebookShowOutput = !options || options->output;
    view.extrasMut().notebookTimeout = options ? options->timeoutSeconds : 0;
    view.extrasMut().notebookKernel = options ? options->kernel : QString();
    view.extrasMut().notebookBlockID = blockID;
    view.extrasMut().notebookOutputs.clear();
    view.extrasMut().notebookOutputTop = view.height;
    view.extrasMut().notebookInputRect = QRectF();

    const NotebookCacheEntry &lookup = ensureNotebookResult(key, language, source, notebookOptionsOf(view));
    view.extrasMut().notebookHasRun = lookup.result.hasRun;
    view.extrasMut().notebookRunning = lookup.running;
    view.extrasMut().notebookOk = lookup.result.ok && lookup.result.kernelError.isEmpty();
    view.extrasMut().notebookCount = lookup.result.executionCount;
    view.extrasMut().notebookStale = false;

    // Output that belongs to what the block said last, shown (dimmed, marked)
    // until it is run again: editing a block must not make its result vanish.
    QString shownKey = key;
    if (lookup.result.hasRun) {
        d->m_notebookBlockKeys.insert(blockID, key);
    } else if (!lookup.running) {
        const QString previous = d->m_notebookBlockKeys.value(blockID);
        const auto earlier = previous.isEmpty() || previous == key ? d->m_notebookCache.constEnd()
                                                                   : d->m_notebookCache.constFind(previous);
        if (earlier != d->m_notebookCache.constEnd() && earlier->result.hasRun
            && earlier->result.kernelError.isEmpty()) {
            shownKey = previous;
            view.extrasMut().notebookStale = true;
            view.extrasMut().notebookHasRun = true;
            view.extrasMut().notebookOk = earlier->result.ok;
            view.extrasMut().notebookCount = earlier->result.executionCount;
        }
    }

    if (lookup.running) {
        appendNotebookMessage(view, tr("Running..."), QStringLiteral("--text-muted"));
        return;
    }
    if (!lookup.result.kernelError.isEmpty()) {
        appendNotebookMessage(view, lookup.result.kernelError, QStringLiteral("--text-error"));
        return;
    }
    if (!view.x().notebookShowOutput)
        return; // `#| output: false`: it ran (or not), nothing is drawn
    if (view.x().notebookStale)
        appendNotebookMessage(view, tr("Outdated: the code changed since this ran"), QStringLiteral("--text-muted"));
    {
        const auto shown = d->m_notebookCache.constFind(shownKey);
        if (shown == d->m_notebookCache.constEnd() || !shown->result.hasRun || shown->result.outputs.isEmpty())
            return; // never run, or ran clean with nothing to show: no output area at all
    }

    // Mutable from here: rendering an image is a cache fill, not a fresh
    // answer, so it belongs to this layout pass rather than to whatever
    // thread delivered the result.
    auto it = d->m_notebookCache.find(shownKey);
    if (it == d->m_notebookCache.end())
        return; // cannot happen (checked just above)
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
        BlockExtras::NotebookOutputCell cell;
        cell.kind = output.kind;
        qreal cellWidth = width;
        qreal cellHeight = 0;

        if (output.kind == NotebookOutput::Kind::Text) {
            cell.layout = buildNotebookTextLayout(
                stripAnsi(output.text), width,
                d->m_theme.color(output.isStderr ? QStringLiteral("--color-orange") : QStringLiteral("--text-normal")));
        } else if (output.kind == NotebookOutput::Kind::Error) {
            // The header names the error ("Exit code 1", "ZeroDivisionError")
            // and the traceback is the body. `text` is the error's evalue; a
            // traceback usually ends with it, so it joins the header only when
            // the body does not already say it - once, not twice.
            QString body = stripAnsi(output.traceback.join(QLatin1Char('\n')));
            while (body.endsWith(QLatin1Char('\n')))
                body.chop(1);
            const QString evalue = output.text.trimmed();
            QString text = output.ename;
            if (!evalue.isEmpty() && !body.contains(evalue))
                text += (text.isEmpty() ? QString() : QStringLiteral(": ")) + evalue;
            if (!body.isEmpty())
                text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + body;
            cell.layout = buildNotebookTextLayout(text, width, d->m_theme.color(QStringLiteral("--text-error")));
        } else {
            cell.image = i < entry.renderedImages.size() ? entry.renderedImages.at(i) : QPixmap();
            // HTML with no litehtml to draw it (or nothing to see in it): the
            // plain text the kernel sent beside it, as a terminal would show.
            if (cell.image.isNull() && output.kind == NotebookOutput::Kind::Html && !output.alt.isEmpty())
                cell.layout = buildNotebookTextLayout(stripAnsi(output.alt), width,
                                                      d->m_theme.color(QStringLiteral("--text-normal")));
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
        view.extrasMut().notebookOutputs.push_back(std::move(cell));
    }
    view.height = y;
}

// --- the run control, in the header strip alongside "Copy" ----------------

QRectF MarkdownEditor::notebookRunRectFor(const BlockView &view, qreal left, qreal top) const
{
    if (!view.notebookEligible || view.piece > 0)
        return {};
    const QFontMetricsF metrics(d->m_theme.uiFont());
    const qreal size = metrics.height();
    if (view.editedListing) {
        // No header strip while the caret is in the listing: the control sits
        // in the panel's top-right corner, on the opening fence line, whose
        // text is short and at the left (its info string would have to run
        // the whole column to reach it).
        qreal fenceLine = 0;
        const QRectF edited = editedListingPanel(view, left, top, &fenceLine);
        const qreal line = fenceLine > 0 ? fenceLine : lineHeightFor(view.span);
        return QRectF(edited.right() - 14 - size, edited.top() + (line - size) / 2, size, size);
    }
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    // The header row every notebook-eligible block gets (view.textTop was
    // set to codeHeaderHeight() for exactly this reason - see layoutBlock).
    const qreal y = panel.top() + (view.textTop + 8 - size) / 2;
    // Left of the copy button's reserved place at the header's right end,
    // never on it, and in the same place whether "Copy" is showing or not.
    const CodeLabel label = codeLabelOf(view, left, top);
    const qreal right = label.slotLeft > 0 ? label.slotLeft - 8 - size : panel.right() - 22 - kFoldHandle - size;
    return QRectF(right, y, size, size);
}

void MarkdownEditor::paintNotebookRunControl(QPainter &painter, const BlockView &view, qreal left,
                                             qreal top) const
{
    const QRectF rect = notebookRunRectFor(view, left, top);
    if (!rect.isValid())
        return;
    painter.save();
    const QColor colour = view.x().notebookRunning ? d->m_theme.color(QStringLiteral("--text-muted"))
                          : !view.x().notebookOk    ? d->m_theme.color(QStringLiteral("--text-error"))
                                                : d->m_theme.color(QStringLiteral("--text-muted"));
    // "square" (running - a click on it interrupts the run), "triangle-alert" (last run failed) or
    // "play" (idle, including "never run yet"). Icons::icon falls back to
    // nothing drawn for a name the icon set does not have, so an icon-less
    // theme still leaves the rect clickable, just blank.
    const QString name = view.x().notebookRunning ? QStringLiteral("square")
                         : !view.x().notebookOk    ? QStringLiteral("triangle-alert")
                                               : QStringLiteral("play");
    const QIcon icon = Icons::icon(name, colour, int(rect.height()));
    if (!icon.isNull())
        icon.paint(&painter, rect.toRect());
    painter.restore();
}

void MarkdownEditor::paintNotebookHostBadge(QPainter &painter, const BlockView &view, qreal left,
                                            qreal top) const
{
    // "wasm" for the WebAssembly tier, then Jupyter's execution count: [3] after a run in a
    // kernel, [*] while one runs there.
    QString text;
    if (view.x().notebookSandboxWasm)
        text = tr("wasm");
    if (!view.x().notebookImplicit.isEmpty() || !view.x().notebookImplicitMain.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral(" ")) + tr("+implicit");
    if (view.x().notebookRunning && view.x().notebookCount > 0)
        text += (text.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("[*]");
    else if (view.x().notebookCount > 0)
        text += (text.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("[%1]").arg(view.x().notebookCount);
    if (text.isEmpty())
        return;
    const QRectF run = notebookRunRectFor(view, left, top);
    if (!run.isValid())
        return;
    QFont font = d->m_theme.uiFont();
    font.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    const QFontMetricsF metrics(font);
    const qreal width = metrics.horizontalAdvance(text);
    const QRectF rect(run.left() - width - 8, run.top(), width, run.height());

    painter.save();
    painter.setFont(font);
    painter.setPen(d->m_theme.color(QStringLiteral("--text-muted")));
    painter.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.restore();
}

void MarkdownEditor::paintNotebookOutput(QPainter &painter, const BlockView &view, qreal left,
                                         qreal top) const
{
    if (view.x().notebookOutputs.empty())
        return;
    painter.save();
    if (view.x().notebookStale)
        painter.setOpacity(0.5); // an earlier run's output, not this code's
    for (const BlockExtras::NotebookOutputCell &cell : view.x().notebookOutputs) {
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
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return info;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.notebookEligible)
        return info;
    info.eligible = true;
    info.hasRun = view.x().notebookHasRun;
    info.running = view.x().notebookRunning;
    info.ok = view.x().notebookOk;
    info.stale = view.x().notebookStale;
    info.executionCount = view.x().notebookCount;
    info.outputCellCount = int(view.x().notebookOutputs.size());
    const qreal left = contentLeft();
    const qreal top = view.y - verticalScrollBar()->value();
    info.runRect = notebookRunRectFor(view, left, top);
    info.labelRect = codeLabelOf(view, left, top).rect;
    return info;
}

QStringList MarkdownEditor::notebookOutputTextAt(qsizetype offset) const
{
    QStringList texts;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return texts;
    for (const BlockExtras::NotebookOutputCell &cell : d->m_blocks[size_t(index)].x().notebookOutputs)
        texts.append(cell.layout ? cell.layout->text().replace(QChar(QChar::LineSeparator), QLatin1Char('\n'))
                                 : QString());
    return texts;
}

int MarkdownEditor::runAllNotebookBlocks()
{
    return runNotebookBlocksUpTo(d->m_text.size());
}

void MarkdownEditor::layOutNotebookBlocks()
{
    if (!d->m_features.notebooks || !d->m_notebookRunner)
        return;
    // The block at the top of the view, and how far into it the view starts, is what stays put (adoptDecorations
    // does the same): real heights for the blocks above it move everything below.
    QScrollBar *bar = verticalScrollBar();
    qsizetype anchorStart = -1;
    qreal anchorInset = 0;
    if (const auto top = std::partition_point(d->m_blocks.cbegin(), d->m_blocks.cend(),
                                              [scroll = qreal(bar->value())](const BlockView &view) {
                                                  return view.bottomY() <= scroll;
                                              });
        top != d->m_blocks.cend() && !top->foldHidden) {
        anchorStart = top->span.range.start;
        anchorInset = bar->value() - top->y;
    }
    bool laid = false;
    for (BlockView &view : d->m_blocks) {
        if (view.laidOut || view.piece != 0)
            continue;
        if (view.span.kind != BlockSpan::Kind::Code && (d->m_decorations.embeds.isEmpty() || !soleEmbedOf(view)))
            continue;
        layoutBlock(view);
        laid = true;
    }
    if (!laid)
        return;
    updateLayout();
    for (int pass = 0; pass < 4 && anchorStart >= 0; ++pass) {
        const BlockView &anchor = d->m_blocks.at(size_t(blockAt(anchorStart)));
        const int wanted = int(anchor.y + anchorInset);
        if (anchor.foldHidden || wanted == bar->value())
            break;
        bar->setValue(wanted);
        updateLayout();
    }
    viewport()->update();
}

int MarkdownEditor::runNotebookBlocksUpTo(qsizetype offset)
{
    layOutNotebookBlocks();
    // Offsets first: a run can open a dialog (a shared note's block asks), and
    // a remote edit applied in its event loop replaces both lists.
    QList<qsizetype> starts;
    for (const CodeFenceSpan &fence : std::as_const(d->m_decorations.codeFences)) {
        if (fence.content.start > offset)
            break; // in document order: nothing past the cursor's block
        starts.append(fence.content.start);
    }
    // A runnable `![[script.py]]` is a block too, in the note's order with the
    // fences; they are run after the fences here, which only matters to a
    // script that needs a variable a fence set - and a fence cannot hand one
    // to a file embed anyway.
    for (const BlockView &view : d->m_blocks) {
        if (view.notebookEligible && view.x().embedPanel && view.span.range.start <= offset)
            starts.append(view.span.range.start);
    }
    // An edit meanwhile (another person's, while a block asked first) moves
    // the blocks: the offsets no longer name them, so the rest is not run.
    const quint64 version = d->m_textVersion;
    int count = 0;
    for (const qsizetype start : std::as_const(starts)) {
        if (d->m_textVersion != version)
            break;
        count += runNotebookBlockAt(start) ? 1 : 0;
    }
    return count;
}

int MarkdownEditor::runNotebookBlocksFrom(qsizetype offset)
{
    layOutNotebookBlocks();
    // Offsets first, for the reason runNotebookBlocksUpTo gives.
    QList<qsizetype> starts;
    for (const CodeFenceSpan &fence : std::as_const(d->m_decorations.codeFences)) {
        if (fence.block.end > offset)
            starts.append(fence.content.start);
    }
    for (const BlockView &view : d->m_blocks) {
        if (view.notebookEligible && view.x().embedPanel && view.span.range.end > offset)
            starts.append(view.span.range.start);
    }
    const quint64 version = d->m_textVersion;
    int count = 0;
    for (const qsizetype start : std::as_const(starts)) {
        if (d->m_textVersion != version)
            break;
        count += runNotebookBlockAt(start) ? 1 : 0;
    }
    return count;
}

int MarkdownEditor::notebookDuplicateOrdinal(const CodeFenceSpan &fence) const
{
    // The code as the block runs it: the lines between the fences, minus the blank ones that open them and the
    // newline that ends them. Compared in place, so a note of thousands of fences allocates nothing here.
    const auto codeOf = [this](const CodeFenceSpan &of) {
        QStringView code = of.content.isValid() ? QStringView(d->m_text).mid(of.content.start, of.content.length())
                                                : QStringView();
        while (code.startsWith(QLatin1Char('\n')))
            code = code.mid(1);
        if (code.endsWith(QLatin1Char('\n')))
            code.chop(1);
        return code;
    };
    // What counts is what a notebook's own reading (Ipynb.cpp's cellsFromMarkdown) takes for a cell: a fence that
    // opens at the start of a line, indented by at most three spaces. One quoted or listed inside a markdown
    // cell stays part of that cell, so the cells' ordinals there and here are the same.
    const auto isCell = [this](const CodeFenceSpan &of) {
        if (of.depth > 0)
            return false;
        int indent = 0;
        for (qsizetype at = of.block.start; at > 0 && d->m_text.at(at - 1) != QLatin1Char('\n'); --at) {
            if (d->m_text.at(at - 1) != QLatin1Char(' ') || ++indent > 3)
                return false;
        }
        return true;
    };
    const QStringView mine = codeOf(fence);
    int count = 0;
    for (const CodeFenceSpan &other : std::as_const(d->m_decorations.codeFences)) {
        if (&other == &fence)
            break;
        if (other.language.trimmed().compare(fence.language.trimmed(), Qt::CaseInsensitive) == 0 && codeOf(other) == mine
            && isCell(other))
            ++count;
    }
    return count;
}

int MarkdownEditor::clearNotebookOutputs()
{
    // Every block, not the ones near the view. A block laid out only now has its lookup still in flight, so
    // the count below is of the outputs this editor knew of.
    layOutNotebookBlocks();
    int cleared = 0;
    for (BlockView &view : d->m_blocks) {
        if (!view.notebookEligible || !d->m_notebookRunner)
            continue;
        NotebookCacheEntry &entry = d->m_notebookCache[view.x().notebookKey];
        const bool had = entry.result.hasRun || view.x().notebookStale;
        NotebookRunOptions options = notebookOptionsOf(view);
        options.clear = true;
        // The sidecar first (the runner), then what is on screen: an emptied
        // result with the generation that makes the late delivery count.
        const QString key = view.x().notebookKey;
        const quint64 generation = ++d->m_notebookGeneration;
        entry.generation = generation;
        entry.running = false;
        entry.pending = false;
        d->m_notebookBlockKeys.remove(view.x().notebookBlockID);
        d->m_notebookRunner(view.x().notebookLanguage, view.x().notebookSource, false, options,
                         [this, lifeline = d->m_lifeline, key, generation](NotebookRunResult result) {
                             lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                                 deliverNotebookResult(key, generation, std::move(result));
                             });
                         });
        cleared += had ? 1 : 0;
    }
    return cleared;
}

QString MarkdownEditor::codeLanguageAt(qsizetype offset) const
{
    for (const CodeFenceSpan &fence : d->m_decorations.codeFences) {
        if (fence.block.start <= offset && offset < fence.block.end)
            return fence.language.trimmed().toLower();
    }
    return {};
}

bool MarkdownEditor::runNotebookBlockAt(qsizetype offset)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return false;
    BlockView &view = d->m_blocks[size_t(index)];
    if (!view.notebookEligible || !d->m_notebookRunner)
        return false;
    // The ordinal is part of the key, and an edit above can change it without this block being laid out again:
    // laid out afresh, it runs (and delivers) under the key its place in the note gives it now.
    if (const CodeFenceSpan *fence = codeFenceOf(view); fence && notebookDuplicateOrdinal(*fence) != view.x().notebookOrdinal) {
        layoutBlock(view);
        if (!view.notebookEligible)
            return false;
    }

    // Exactly what appendNotebookOutput last laid this block out with -
    // a fenced block's own fence/content, or a runnable embed's file - so a
    // click always runs what is actually on screen, from the one place that
    // ever derives it, instead of a second re-derivation here going stale
    // against the first.
    const QString language = view.x().notebookLanguage;
    const QString source = view.x().notebookSource;
    const QString key = view.x().notebookKey;
    NotebookRunOptions options = notebookOptionsOf(view);

    NotebookCacheEntry &entry = d->m_notebookCache[key]; // inserted fresh if this is the first run
    if (entry.running) {
        // The stop control: a second click on a running block interrupts it.
        emit notebookInterruptRequested(language);
        return true;
    }
    entry.running = true;
    entry.pending = false;
    const quint64 generation = ++d->m_notebookGeneration;
    entry.generation = generation;
    // input() in the code: the prompt appears under this block (the runner calls this from its own thread).
    options.onInput = [this, lifeline = d->m_lifeline, key, generation](const NotebookInputPrompt &prompt,
                                                                      const NotebookInputReply &reply) {
        lifeline->post(this, [this, key, generation, prompt, reply] {
            showNotebookInput(key, generation, prompt, reply);
        });
    };

    view.extrasMut().notebookRunning = true;
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();

    d->m_notebookRunner(language, source, /*forceRun=*/true, options,
                     [this, lifeline = d->m_lifeline, key, generation](NotebookRunResult result) {
                         lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                             deliverNotebookResult(key, generation, std::move(result));
                         });
                     });
    return true;
}

} // namespace noteAppC
