/// ```mermaid fences, drawn by an external tool (MermaidBlock.h has the
/// contract). Everything here runs off the UI thread except the closure
/// that CodeBlockHook.cpp calls, which only reads the theme, takes two
/// numbers off the display and queues a job.

#include "MermaidBlock.h"

#include "MermaidCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>

#include <atomic>
#include <memory>

namespace noteAppC {

namespace {

constexpr int kRunMilliseconds = 5000; ///< a diagram that takes longer is killed
constexpr int kMaxPixels = 8192;       ///< one side of the raster, whatever the width and ratio
constexpr int kCacheDays = 30;                  ///< a cached SVG not used for this long is dropped
constexpr qint64 kCacheBytes = 50 * 1024 * 1024; ///< and the oldest go first beyond this much

/// What a bare name or a path in the setting means, or empty when no such
/// executable exists. A desktop launcher's PATH often lacks the cargo bin
/// directory `cargo install` puts mmdr in, so after PATH come $CARGO_HOME/bin,
/// ~/.cargo/bin and ~/.local/share/cargo/bin (the XDG spelling of it).
QString resolveTool(const QString &setting)
{
    QString name = setting.trimmed();
    if (name.isEmpty())
        name = QStringLiteral("mmdr");
    if (name.startsWith(QLatin1String("~/")))
        name = QDir::homePath() + name.mid(1);
    if (name.contains(QLatin1Char('/'))) {
        const QFileInfo info(name);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    QString found = QStandardPaths::findExecutable(name);
    if (!found.isEmpty())
        return found;
    QStringList cargo;
    if (const QString home = qEnvironmentVariable("CARGO_HOME"); !home.isEmpty())
        cargo << home + QStringLiteral("/bin");
    cargo << QDir::homePath() + QStringLiteral("/.cargo/bin") << QDir::homePath() + QStringLiteral("/.local/share/cargo/bin");
    return QStandardPaths::findExecutable(name, cargo);
}

QString firstLine(const QString &text)
{
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            return trimmed.left(200);
    }
    return QString();
}

/// One run of the tool. Empty `error` means `svg` is the diagram.
struct Made {
    QByteArray svg;
    QString error;
};

/// `mmdr -i - -c config.json`: the source on stdin, the SVG on stdout, so the
/// only file is the config (which mmdr takes by path alone). Not `-o -`: 0.3.1
/// writes that to a file named "-".
Made runTool(const QString &tool, const QString &source, const QByteArray &config)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    const QString configPath = dir.filePath(QStringLiteral("config.json"));
    QFile file(configPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(config) != config.size())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary file")};
    file.close();

    QProcess process;
    process.start(tool, {QStringLiteral("-i"), QStringLiteral("-"), QStringLiteral("-c"), configPath});
    if (!process.waitForStarted(kRunMilliseconds))
        return {{}, QCoreApplication::translate("MermaidBlock", "could not start %1").arg(tool)};
    process.write(source.toUtf8());
    process.closeWriteChannel();
    if (!process.waitForFinished(kRunMilliseconds)) {
        process.kill();
        process.waitForFinished(1000);
        return {{}, QCoreApplication::translate("MermaidBlock", "timed out after %1 s").arg(kRunMilliseconds / 1000)};
    }
    const QByteArray svg = process.readAllStandardOutput();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && !svg.isEmpty())
        return {svg, QString()};
    const QString message = firstLine(QString::fromUtf8(process.readAllStandardError()));
    return {{}, !message.isEmpty() ? message : QCoreApplication::translate("MermaidBlock", "exit code %1").arg(process.exitCode())};
}

/// What a renderer remembers between fences: the tool's resolved path (a
/// miss too) and every outcome by key. Shared by the closure and its jobs.
struct State {
    QString setting;
    QMutex mutex;
    bool resolved = false;
    QString tool; ///< empty once resolved: not installed
    QHash<QByteArray, Made> memo;
};

using Result = MarkdownEditor::CodeBlockResult;

/// The source in a listing and one dim line under it. The listing is a
/// `text` fence, never `mermaid`: the Markdown is drawn by a nested editor
/// that must not ask for a diagram again.
Result sourceWithHint(const QString &source, QString hint)
{
    QString escaped;
    for (const QChar c : std::as_const(hint)) {
        if (QStringLiteral("\\`*_<>[]").contains(c))
            escaped += QLatin1Char('\\');
        escaped += c;
    }
    return {true, QStringLiteral("~~~~~~text\n") + source.trimmed() + QStringLiteral("\n~~~~~~\n\n*") + escaped + QLatin1Char('*'),
            QImage()};
}

/// The SVG at `width` logical pixels (never wider than it is, never
/// taller than kMaxPixels allows) and `ratio` device pixels to each.
Result rasterise(const QByteArray &svg, const QString &source, int width, qreal ratio)
{
    QSvgRenderer renderer(svg);
    QSizeF natural = renderer.isValid() ? QSizeF(renderer.defaultSize()) : QSizeF();
    if (natural.isEmpty() && renderer.isValid())
        natural = renderer.viewBoxF().size();
    if (natural.isEmpty())
        return sourceWithHint(source, QCoreApplication::translate("MermaidBlock", "Mermaid: the tool's SVG could not be drawn"));
    const qreal logicalWidth = qMin<qreal>(qMax(width, 1), natural.width());
    const qreal logicalHeight = natural.height() * logicalWidth / natural.width();
    qreal scale = qMax<qreal>(ratio, 1);
    scale = qMin(scale, kMaxPixels / qMax(logicalWidth, logicalHeight));
    const QSize pixels(qMax(1, qRound(logicalWidth * scale)), qMax(1, qRound(logicalHeight * scale)));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(QPointF(), QSizeF(pixels)));
    }
    image.setDevicePixelRatio(pixels.width() / logicalWidth);
    return {false, QString(), std::move(image)};
}

/// The whole job: memo, then disk, then the tool. On a pool thread.
Result make(const std::shared_ptr<State> &state, const QString &source, const QByteArray &config, int width,
            qreal ratio)
{
    const QString cacheFile = mermaidCachePath(source, config);
    const QByteArray key = cacheFile.toUtf8(); // the hash is in the name
    Made made;
    bool have = false;
    {
        QMutexLocker lock(&state->mutex);
        if (const auto it = state->memo.constFind(key); it != state->memo.cend()) {
            made = it.value();
            have = true;
        }
    }
    if (!have) {
        made.svg = mermaidCachedSVG(cacheFile);
        have = !made.svg.isEmpty();
    }
    if (!have) {
        QString tool;
        {
            QMutexLocker lock(&state->mutex);
            if (!state->resolved) {
                state->tool = resolveTool(state->setting);
                state->resolved = true;
            }
            tool = state->tool;
        }
        if (tool.isEmpty()) {
            const QString name = state->setting.trimmed();
            return sourceWithHint(source, name.isEmpty() || name == QLatin1String("mmdr")
                                              ? QCoreApplication::translate("MermaidBlock", "Mermaid: install mmdr to render")
                                              : QCoreApplication::translate("MermaidBlock", "Mermaid: %1 was not found").arg(name));
        }
        made = runTool(tool, source, config);
        if (made.error.isEmpty())
            mermaidCacheStore(cacheFile, made.svg);
    }
    {
        QMutexLocker lock(&state->mutex);
        if (state->memo.size() >= 64)
            state->memo.clear(); // ponytail: a flush, not an LRU; the disk copy answers the next ask
        state->memo.insert(key, made);
    }
    if (!made.error.isEmpty())
        return sourceWithHint(source, QCoreApplication::translate("MermaidBlock", "Mermaid: %1").arg(made.error));
    return rasterise(made.svg, source, width, ratio);
}

/// One thread, not the global pool's (parsing, search) while a tool runs for
/// seconds: diagrams queue, and a second ask for a text already being made
/// finds it in the memo instead of starting the tool twice. ponytail: a hung
/// tool holds the queue for its 5 s; widen it with per-key locking if that is felt.
QThreadPool *pool()
{
    static QThreadPool instance;
    static const bool once = (instance.setMaxThreadCount(1), true);
    Q_UNUSED(once);
    return &instance;
}

} // namespace

MarkdownEditor::CodeBlockRenderer mermaidRenderer(const QString &tool, std::function<Theme()> theme)
{
    auto state = std::make_shared<State>();
    state->setting = tool;
    return [state, theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                              const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        // The UI thread's part: two reads, one queued job.
        const QByteArray config = mermaidConfig(theme());
        const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
        (void)QtConcurrent::run(pool(), [state, source, config, width, ratio, deliver] {
            deliver(make(state, source, config, width, ratio));
            // Once per process, after the first diagram is out, never at startup.
            static std::atomic_flag pruned;
            if (!pruned.test_and_set())
                pruneMermaidCache(mermaidCacheDirectory(), kCacheDays, kCacheBytes);
        });
    };
}

MarkdownEditor::CodeBlockRenderer mermaidCachedRenderer(Theme theme)
{
    return [theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                       const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        const QByteArray svg = mermaidCachedSVG(source, theme);
        deliver(svg.isEmpty() ? Result{true, QString(), QImage()} : rasterise(svg, source, width, 2));
    };
}

} // namespace noteAppC
