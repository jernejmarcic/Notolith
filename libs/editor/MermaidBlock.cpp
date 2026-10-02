/// ```mermaid fences, drawn by an external tool (MermaidBlock.h has the
/// contract). Everything here runs off the UI thread except the closure
/// that CodeBlockHook.cpp calls, which only reads the theme, takes two
/// numbers off the display and queues a job.

#include "MermaidBlock.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>

#include <memory>

namespace noteAppC {

namespace {

constexpr int kRunMilliseconds = 5000; ///< a diagram that takes longer is killed
constexpr int kMaxPixels = 8192;       ///< one side of the raster, whatever the width and ratio

/// The colours a diagram is asked for, copied off the Theme on the UI thread
/// so a job never touches it.
struct Palette {
    bool dark = true;
    QColor background, panel, text, muted, accent, border;

    static Palette of(const Theme &theme)
    {
        Palette p;
        p.dark = theme.isDark();
        p.background = theme.color(QStringLiteral("--background-primary"));
        p.panel = theme.color(QStringLiteral("--background-secondary"));
        p.text = theme.color(QStringLiteral("--text-normal"));
        p.muted = theme.color(QStringLiteral("--text-muted"));
        p.accent = theme.color(QStringLiteral("--interactive-accent"));
        p.border = theme.color(QStringLiteral("--background-modifier-border"));
        return p;
    }
    /// Part of the cache key: another scheme is another picture.
    QString signature() const
    {
        return QString::number(dark) + background.name() + panel.name() + text.name() + muted.name()
               + accent.name() + border.name();
    }
};

QString cacheRoot()
{
    // The same spelling as TexCompiler.cpp's: QStandardPaths folds the
    // application name in, and the cache should survive how noteAppC is run.
    QString base = qEnvironmentVariable("XDG_CACHE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/noteAppC/mermaid");
}

/// What a bare name or a path in the setting means, or empty when no such
/// executable exists. A desktop launcher's PATH often lacks ~/.cargo/bin,
/// where `cargo install` puts mmdr, so that is looked in as well.
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
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(name, {QDir::homePath() + QStringLiteral("/.cargo/bin")});
    return found;
}

/// Mermaid's own config shape (`theme`, `themeVariables`), in the app's
/// colours. ponytail: mmdr's config keys are unverified, so a tool that
/// rejects this is run again without it (runTool), and adaptColours below
/// covers one that accepts it and ignores the colours.
QByteArray configFor(const Palette &p)
{
    QJsonObject vars{
        {QStringLiteral("darkMode"), p.dark},
        {QStringLiteral("background"), p.background.name()},
        {QStringLiteral("primaryColor"), p.panel.name()},
        {QStringLiteral("primaryTextColor"), p.text.name()},
        {QStringLiteral("primaryBorderColor"), p.accent.name()},
        {QStringLiteral("secondaryColor"), p.panel.name()},
        {QStringLiteral("tertiaryColor"), p.background.name()},
        {QStringLiteral("lineColor"), p.muted.name()},
        {QStringLiteral("textColor"), p.text.name()},
        {QStringLiteral("mainBkg"), p.panel.name()},
        {QStringLiteral("nodeBorder"), p.accent.name()},
        {QStringLiteral("clusterBkg"), p.background.name()},
        {QStringLiteral("clusterBorder"), p.border.name()},
        {QStringLiteral("edgeLabelBackground"), p.background.name()},
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("theme"), p.dark ? QStringLiteral("dark") : QStringLiteral("default")},
                                     {QStringLiteral("themeVariables"), vars}})
        .toJson(QJsonDocument::Compact);
}

/// A dark theme and a light diagram: re-lights every colour in the SVG,
/// keeping hue and saturation, so white becomes the editor's background and
/// black its text. "Light diagram" is judged by the tool having ignored the
/// colours asked for (neither the node fill nor the text colour is in the
/// file); a light theme keeps Mermaid's own light default as it is.
/// ponytail: colour literals only (hex, rgb(), white, black) - a colour set
/// through a named colour or a `currentColor` stays; upgrade when mmdr's
/// output shows one.
QString adaptColours(QString svg, const Palette &p)
{
    if (!p.dark || svg.contains(p.panel.name(), Qt::CaseInsensitive)
        || svg.contains(p.text.name(), Qt::CaseInsensitive))
        return svg;
    const qreal backgroundLightness = p.background.lightnessF();
    const auto relight = [&](QColor colour) {
        const qreal lightness = colour.lightnessF();
        colour = colour.toHsl();
        colour.setHslF(colour.hslHueF() < 0 ? 0 : colour.hslHueF(), colour.hslSaturationF(),
                       backgroundLightness + (1 - lightness) * (1 - backgroundLightness), colour.alphaF());
        return colour.name();
    };
    static const QRegularExpression colour(
        QStringLiteral("#[0-9a-fA-F]{6}\\b|#[0-9a-fA-F]{3}\\b|rgb\\(\\s*\\d+\\s*,\\s*\\d+\\s*,\\s*\\d+\\s*\\)|"
                       "(?<=[\"':\\s])(?:white|black)(?=[\"';\\s])"));
    QString out;
    qsizetype last = 0;
    for (auto it = colour.globalMatch(svg); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        QColor c;
        const QString text = m.captured();
        if (text.startsWith(QLatin1String("rgb("))) {
            const QStringList parts = text.mid(4, text.size() - 5).split(QLatin1Char(','));
            c = QColor(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
        } else {
            c = QColor(text);
        }
        if (!c.isValid())
            continue;
        out += QStringView(svg).mid(last, m.capturedStart() - last);
        out += relight(c);
        last = m.capturedEnd();
    }
    out += QStringView(svg).mid(last);
    return out;
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

Made runTool(const QString &tool, const QString &source, const Palette &palette)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    const QString in = dir.filePath(QStringLiteral("diagram.mmd"));
    const QString out = dir.filePath(QStringLiteral("diagram.svg"));
    const QString config = dir.filePath(QStringLiteral("config.json"));
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write(in, source.toUtf8()) || !write(config, configFor(palette)))
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary file")};

    Made made;
    for (const bool withConfig : {true, false}) {
        QFile::remove(out);
        QProcess process;
        QStringList args{QStringLiteral("-i"), in, QStringLiteral("-o"), out};
        if (withConfig)
            args << QStringLiteral("-c") << config;
        process.start(tool, args);
        process.closeWriteChannel();
        if (!process.waitForStarted(kRunMilliseconds)) {
            made.error = QCoreApplication::translate("MermaidBlock", "could not start %1").arg(tool);
            return made;
        }
        if (!process.waitForFinished(kRunMilliseconds)) {
            process.kill();
            process.waitForFinished(1000);
            made.error = QCoreApplication::translate("MermaidBlock", "timed out after %1 s").arg(kRunMilliseconds / 1000);
            return made;
        }
        QFile svg(out);
        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && svg.open(QIODevice::ReadOnly)) {
            made.svg = svg.readAll();
            made.error.clear();
            if (!made.svg.isEmpty())
                return made;
        }
        // ponytail: any failure is tried once more without the config, in
        // case this version of mmdr does not take it; a real syntax error
        // costs a second ~4 ms run and reports the second run's message.
        const QString message = firstLine(QString::fromUtf8(process.readAllStandardError()));
        made.error = !message.isEmpty() ? message
                                        : QCoreApplication::translate("MermaidBlock", "exit code %1").arg(process.exitCode());
    }
    return made;
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
Result make(const std::shared_ptr<State> &state, const QString &source, const Palette &palette, int width,
            qreal ratio)
{
    const QByteArray key = QCryptographicHash::hash(
                               (QStringLiteral("1|") + palette.signature() + QLatin1Char('|') + source).toUtf8(),
                               QCryptographicHash::Sha1)
                               .toHex();
    Made made;
    bool have = false;
    {
        QMutexLocker lock(&state->mutex);
        if (const auto it = state->memo.constFind(key); it != state->memo.cend()) {
            made = it.value();
            have = true;
        }
    }
    const QString cacheFile = cacheRoot() + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".svg");
    if (!have) {
        QFile disk(cacheFile);
        if (disk.open(QIODevice::ReadOnly)) {
            made.svg = disk.readAll();
            have = !made.svg.isEmpty();
        }
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
        made = runTool(tool, source, palette);
        if (made.error.isEmpty()) {
            made.svg = adaptColours(QString::fromUtf8(made.svg), palette).toUtf8();
            if (QDir().mkpath(cacheRoot())) {
                QSaveFile file(cacheFile);
                if (file.open(QIODevice::WriteOnly) && file.write(made.svg) == made.svg.size())
                    file.commit();
            }
        }
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

QString mermaidCacheDirectory()
{
    return cacheRoot();
}

MarkdownEditor::CodeBlockRenderer mermaidRenderer(const QString &tool, std::function<Theme()> theme)
{
    auto state = std::make_shared<State>();
    state->setting = tool;
    return [state, theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                              const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        // The UI thread's part: two reads, one queued job.
        const Palette palette = Palette::of(theme());
        const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
        (void)QtConcurrent::run(pool(), [state, source, palette, width, ratio, deliver] {
            deliver(make(state, source, palette, width, ratio));
        });
    };
}

} // namespace noteAppC
