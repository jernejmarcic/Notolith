// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
/// ```markmap fences (MarkmapBlock.h has the contract). The layout and the
/// SVG are libs/render/Markmap.cpp's; this is the editor's side: a pool
/// thread, the raster, and the fold state a click on a circle changes. Nothing
/// is kept on disk: publish and PDF export draw a map from its text themselves.

#include "MarkmapBlock.h"

#include "Markmap.h"
#include "MermaidBlock.h"
#include "MermaidCache.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QThreadPool>
#include <QtConcurrent>

#include <memory>

namespace noteAppC {

namespace {

constexpr int kRememberedFolds = 256; ///< sources whose folding is kept

using Result = MarkdownEditor::CodeBlockResult;

struct State {
    QMutex mutex;
    QHash<QByteArray, QSet<int>> toggled; ///< by a hash of the fence's text: the nodes the view flipped
};

QByteArray hashOf(const QString &source)
{
    return QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha1);
}

QThreadPool *pool()
{
    static QThreadPool instance;
    static const bool once = [] {
        instance.setMaxThreadCount(1);
        return true;
    }();
    Q_UNUSED(once);
    return &instance;
}

/// The map for the source as the view has folded it, as an answer. On a pool thread.
void draw(const std::shared_ptr<State> &state, const QString &source, const QByteArray &config, int width, qreal ratio,
          const std::function<void(Result)> &deliver)
{
    QSet<int> toggled;
    {
        QMutexLocker lock(&state->mutex);
        toggled = state->toggled.value(hashOf(source));
    }
    const MarkmapDrawn drawn = drawMarkmap(parseMarkmap(source), markmapLook(config), toggled);
    qreal scale = 1;
    Result result{false, QString(), rasteriseSVG(drawn.svg, width, ratio, &scale)};
    if (result.image.isNull()) {
        deliver({true, QString(), QImage()}); // nothing drawable: the listing shows
        return;
    }
    for (const MarkmapToggle &toggle : drawn.toggles)
        result.hotspots.append({QRectF(toggle.rect.topLeft() * scale, toggle.rect.size() * scale), toggle.node});
    // What this picture shows is what a click flips, not what the set holds by now: a second view of the
    // same note (a split) may still show the old picture, and its click has to agree with what it saw.
    result.activate = [state, source, config, width, ratio, shown = toggled](int node, const std::function<void(Result)> &answer) {
        {
            QMutexLocker lock(&state->mutex);
            const QByteArray key = hashOf(source);
            if (!state->toggled.contains(key) && state->toggled.size() >= kRememberedFolds)
                state->toggled.clear(); // the other sources' folds go, never the one being clicked
            QSet<int> &folds = state->toggled[key];
            if (shown.contains(node))
                folds.remove(node);
            else
                folds.insert(node);
        }
        (void)QtConcurrent::run(pool(), [state, source, config, width, ratio, answer] { draw(state, source, config, width, ratio, answer); });
    };
    deliver(std::move(result));
}

} // namespace

MarkdownEditor::CodeBlockRenderer markmapRenderer(std::function<Theme()> theme)
{
    auto state = std::make_shared<State>();
    return [state, theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                             const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        // The UI thread's part: the theme, the display's ratio, one queued job.
        const QByteArray config = diagramConfig(DiagramKind::Markmap, theme());
        const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
        (void)QtConcurrent::run(pool(), [state, source, config, width, ratio, deliver] { draw(state, source, config, width, ratio, deliver); });
    };
}

} // namespace noteAppC
