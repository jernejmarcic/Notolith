/// ```mermaid fences, drawn by an external tool (MermaidBlock.h has the
/// contract). Everything here runs off the UI thread except the closure
/// that CodeBlockHook.cpp calls, which only reads the theme, takes two
/// numbers off the display and queues a job.

#include "MermaidBlock.h"

#include "MarkmapBlock.h"

#include "Bubblewrap.h"
#include "SyscallFilter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>

#include <atomic>
#include <memory>

#include <sys/resource.h>
#include <unistd.h>

namespace noteAppC {

namespace {

constexpr int kRunMilliseconds = 5000; ///< a diagram that takes longer is killed
constexpr int kSandboxedMilliseconds = 20000; ///< ... for the tools that start slowly (a JVM, TeX), and each of TeX's two steps
constexpr rlim_t kAddressSpaceBytes = 4ULL * 1024 * 1024 * 1024; ///< the tools' RLIMIT_AS
constexpr rlim_t kOutputBytes = 16ULL * 1024 * 1024; ///< the most a render may write or print: a bigger SVG is not a diagram
constexpr rlim_t kJVMDataBytes = 2ULL * 1024 * 1024 * 1024;      ///< the JVM's RLIMIT_DATA: heap 512 MB, metaspace 256 MB, code cache, malloc
/// The JVM's own caps (JAVA_TOOL_OPTIONS reaches any java the launcher script runs): a diagram needs well under 100 MB.
const char kJVMOptions[] = "JAVA_TOOL_OPTIONS=-Xmx512m -XX:MaxMetaspaceSize=256m -XX:MaxDirectMemorySize=128m";
constexpr int kKindCount = 5; ///< the arrays below hold one entry per DiagramKind
static_assert(int(DiagramKind::Markmap) + 1 == kKindCount, "a new DiagramKind: grow kKindCount (the pools and prune flags)");
constexpr int kMaxPixels = 8192;       ///< one side of the raster, whatever the width and ratio
constexpr int kCacheDays = 30;                  ///< a cached SVG not used for this long is dropped
constexpr qint64 kCacheBytes = 50 * 1024 * 1024; ///< and the oldest go first beyond this much

/// What a bare name or a path in the setting means, or empty when no such
/// executable exists. A desktop launcher's PATH often lacks the cargo bin
/// directory `cargo install` puts mmdr in, so after PATH come $CARGO_HOME/bin,
/// ~/.cargo/bin and ~/.local/share/cargo/bin (the XDG spelling of it).
QString resolveTool(const QString &setting, const QString &defaultName)
{
    QString name = setting.trimmed();
    if (name.isEmpty())
        name = defaultName;
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

QString lastLine(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (!it->trimmed().isEmpty())
            return it->trimmed().left(200);
    }
    return QString();
}

/// One run of the tool. Empty `error` means `svg` is the diagram.
struct Made {
    QByteArray svg;
    QString error;
};

/// One run of a tool inside bubblewrap's sandbox (sandbox/Bubblewrap.h), in
/// `dir`, the only place it can write. `failure` says why it did not run to
/// the end (no bwrap, no start, timed out, too much output); else `code` is its exit code.
struct Ran {
    int code = -1;
    QByteArray out, err;
    QString failure;
};

/// A CPU ceiling, a memory one (address space, except for a JVM, which reserves far more than it uses:
/// RLIMIT_AS breaks it, so its data segment, which counts what it commits, with the heap flags on top), a
/// file size and a count of open files, in the child; a wall-clock limit and an output cap in run().
sandbox::Limits limitsFor(bool jvm)
{
    sandbox::Limits limits;
    limits.cpuSeconds = 25;
    (jvm ? limits.data : limits.addressSpace) = jvm ? kJVMDataBytes : kAddressSpaceBytes;
    limits.fileSize = kOutputBytes;
    limits.openFiles = 256;
    return limits;
}

/// No bubblewrap (or no syscall filter), no run: the tool is a parser of a note's text, and a note is anyone's.
Ran runSandboxed(const QString &dir, const QString &program, const QStringList &arguments, const QStringList &environment,
                 const QByteArray &input, bool jvm, int milliseconds = kSandboxedMilliseconds)
{
    const sandbox::Command command = sandbox::bubblewrapCommand(dir, program, arguments, environment);
    const sandbox::Outcome outcome = sandbox::run(command, limitsFor(jvm), input, milliseconds, kOutputBytes);
    Ran ran;
    ran.code = outcome.code;
    ran.out = outcome.out;
    ran.err = outcome.err;
    ran.failure = outcome.failure;
    return ran;
}

Made fromRan(const Ran &ran, const QString &message)
{
    if (!ran.failure.isEmpty())
        return {{}, ran.failure};
    if (ran.code == 0 && !ran.out.isEmpty())
        return {ran.out, QString()};
    return {{}, !message.isEmpty() ? message : QCoreApplication::translate("MermaidBlock", "exit code %1").arg(ran.code)};
}

/// `mmdr -i - -c config.json`: the source on stdin, the SVG on stdout, so the
/// only file is the config (which mmdr takes by path alone; the sandbox binds
/// its directory at the same path). Not `-o -`: 0.3.1 writes that to a file
/// named "-". In the sandbox like the other tools, as a note's fence is its
/// input; where there is no bubblewrap it does not run, and the fence shows its source.
Made runMermaid(const QString &tool, const QString &source, const QByteArray &config)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    const QString configPath = dir.filePath(QStringLiteral("config.json"));
    QFile file(configPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(config) != config.size())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary file")};
    file.close();
    const Ran ran = runSandboxed(dir.path(), tool, {QStringLiteral("-i"), QStringLiteral("-"), QStringLiteral("-c"), configPath}, {},
                                 source.toUtf8(), false, kRunMilliseconds);
    return fromRan(ran, firstLine(QString::fromUtf8(ran.err)));
}

QJsonObject colours(const QByteArray &config)
{
    return QJsonDocument::fromJson(config).object();
}

/// `plantuml -tsvg -pipe`: the source on stdin, the SVG on stdout. A source
/// without @start... is wrapped in @startuml; PlantUML's own SANDBOX profile
/// refuses !include of a file outside its directory and of a URL, on top of
/// what the sandbox does. Its errors come as an error diagram and exit code
/// 200 with the message last on stderr.
Made runPlantUML(const QString &tool, const QString &source, const QByteArray &config)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    QString text = source;
    if (!text.contains(QLatin1String("@start")))
        text = QStringLiteral("@startuml\n") + text + QStringLiteral("\n@enduml\n");
    QStringList arguments = {QStringLiteral("-tsvg"), QStringLiteral("-pipe"), QStringLiteral("-charset"), QStringLiteral("UTF-8")};
    if (colours(config).value(QStringLiteral("dark")).toBool())
        arguments << QStringLiteral("--dark-mode");
    const Ran ran = runSandboxed(dir.path(), tool, arguments, {QStringLiteral("PLANTUML_SECURITY_PROFILE=SANDBOX"), QString::fromLatin1(kJVMOptions)}, text.toUtf8(), true);
    return fromRan(ran, lastLine(QString::fromUtf8(ran.err)));
}

/// `dot -Tsvg`: the theme's colours (background too, so the SVG is a card like
/// Mermaid's and PlantUML's when publish or PDF export find it in another
/// theme) as defaults, declared at the top of the
/// graph so the graph's own attributes override them (dot's -G/-N/-E flags
/// would override the graph's `node [color=...]`), on the line of the first
/// `{` so error line numbers stay the source's. A source with no `{` is
/// left as it is for dot to reject.
Made runGraphviz(const QString &tool, const QString &source, const QByteArray &config)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    const QString text = colours(config).value(QStringLiteral("text")).toString();
    const QString background = colours(config).value(QStringLiteral("background")).toString();
    QString themed = source;
    bool quoted = false;
    for (qsizetype i = 0; i < source.size(); ++i) {
        if (source.at(i) == QLatin1Char('"') && (i == 0 || source.at(i - 1) != QLatin1Char('\\')))
            quoted = !quoted;
        else if (source.at(i) == QLatin1Char('{') && !quoted) {
            themed.insert(i + 1, QStringLiteral("bgcolor=\"%2\"; color=\"%1\"; fontcolor=\"%1\"; node [color=\"%1\" fontcolor=\"%1\"]; "
                                                "edge [color=\"%1\" fontcolor=\"%1\"]; ")
                                     .arg(text, background));
            break;
        }
    }
    const Ran ran = runSandboxed(dir.path(), tool, {QStringLiteral("-Tsvg")}, {}, themed.toUtf8(), false);
    return fromRan(ran, firstLine(QString::fromUtf8(ran.err)));
}

/// TikZJax's block: `\usepackage` / `\usetikzlibrary` lines, then
/// `\begin{document}...\end{document}`; or a bare tikzpicture. Either way a
/// standalone document.
QByteArray tikzDocument(const QString &source)
{
    QString preamble, body = source;
    const qsizetype begin = source.indexOf(QLatin1String("\\begin{document}"));
    if (begin >= 0) {
        preamble = source.left(begin);
        body = source.mid(begin + 16);
        if (const qsizetype end = body.lastIndexOf(QLatin1String("\\end{document}")); end >= 0)
            body.truncate(end);
    }
    return (QStringLiteral("\\documentclass[tikz,border=2pt]{standalone}\n") + preamble + QStringLiteral("\n\\begin{document}\n") + body
            + QStringLiteral("\n\\end{document}\n"))
        .toUtf8();
}

/// The SVG as a card in the theme's background (dvisvgm's viewBox starts
/// off the origin), like Mermaid's and PlantUML's, which carry their own: one
/// drawn for another theme is then a card, not lines invisible on a page.
/// On a dark theme black - the SVG's default, which dvisvgm leaves implicit,
/// and the explicit blacks - becomes the text colour first (TikZJax's
/// "invert" option). Other colours stay.
QByteArray themeTikZ(QByteArray svg, const QJsonObject &theme)
{
    static const QRegularExpression black(QStringLiteral("(fill|stroke)=(['\"])(#000000|#000|black)\\2"));
    static const QRegularExpression viewBox(QStringLiteral("viewBox=['\"]([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)['\"]"));
    QString markup = QString::fromUtf8(svg);
    const QString text = theme.value(QStringLiteral("text")).toString();
    if (theme.value(QStringLiteral("dark")).toBool()) {
        markup.replace(black, QStringLiteral("\\1=\\2") + text + QStringLiteral("\\2"));
        markup.replace(QStringLiteral("<svg "), QStringLiteral("<svg fill='") + text + QStringLiteral("' "));
    }
    const QRegularExpressionMatch box = viewBox.match(markup);
    const qsizetype tagEnd = markup.indexOf(QLatin1Char('>'), markup.indexOf(QLatin1String("<svg ")));
    if (box.hasMatch() && tagEnd > 0)
        markup.insert(tagEnd + 1, QStringLiteral("<rect x='%1' y='%2' width='%3' height='%4' fill='%5'/>")
                                      .arg(box.captured(1), box.captured(2), box.captured(3), box.captured(4),
                                           theme.value(QStringLiteral("background")).toString()));
    return markup.toUtf8();
}

/// `latex` then `dvisvgm --no-fonts`, each in the sandbox, in one temporary
/// directory. The sandbox is what keeps \input to the system's files (TeX
/// Live 2026's openin_any does nothing) and -no-shell-escape keeps \write18
/// off. dvisvgm is looked for next to `tool`, then on PATH.
Made runTikZ(const QString &tool, const QString &source, const QByteArray &config)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary directory")};
    QString dvisvgm = QFileInfo(QFileInfo(tool).absolutePath() + QStringLiteral("/dvisvgm")).isExecutable()
                          ? QFileInfo(tool).absolutePath() + QStringLiteral("/dvisvgm")
                          : QStandardPaths::findExecutable(QStringLiteral("dvisvgm"));
    if (dvisvgm.isEmpty())
        return {{}, QCoreApplication::translate("MermaidBlock", "dvisvgm was not found")};
    QFile cell(dir.filePath(QStringLiteral("cell.tex")));
    if (!cell.open(QIODevice::WriteOnly) || cell.write(tikzDocument(source)) < 0)
        return {{}, QCoreApplication::translate("MermaidBlock", "no temporary file")};
    cell.close();

    const Ran tex = runSandboxed(dir.path(), tool,
                                 {QStringLiteral("-interaction=nonstopmode"), QStringLiteral("-halt-on-error"),
                                  QStringLiteral("-no-shell-escape"), QStringLiteral("-no-parse-first-line"), QStringLiteral("cell.tex")},
                                 {QStringLiteral("openout_any=p")}, {}, false); // as TexCompiler: not a texmf.cnf's choice
    if (!tex.failure.isEmpty() || tex.code != 0) {
        // The log's first "! ..." line is TeX's own error.
        QString message;
        for (const QString &line : QString::fromUtf8(tex.out).split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1Char('!'))) {
                message = line.mid(1).trimmed().left(200);
                break;
            }
        }
        return {{}, !tex.failure.isEmpty() ? tex.failure : message.isEmpty() ? QCoreApplication::translate("MermaidBlock", "exit code %1").arg(tex.code) : message};
    }
    const Ran convert = runSandboxed(dir.path(), dvisvgm,
                                     {QStringLiteral("--no-fonts"), QStringLiteral("--exact-bbox"), QStringLiteral("--stdout"),
                                      QStringLiteral("cell.dvi")},
                                     {}, {}, false);
    Made made = fromRan(convert, firstLine(QString::fromUtf8(convert.err)));
    if (made.error.isEmpty())
        made.svg = themeTikZ(made.svg, colours(config));
    return made;
}

/// What differs between the kinds: the words in a hint, the tool a bare
/// setting means, and how it is run.
struct Kind {
    const char *label;
    const char *defaultTool;
    Made (*run)(const QString &tool, const QString &source, const QByteArray &config);
};

const Kind &kindOf(DiagramKind kind)
{
    static const Kind kinds[] = {{"Mermaid", "mmdr", runMermaid},
                                 {"PlantUML", "plantuml", runPlantUML},
                                 {"TikZ", "latex", runTikZ},
                                 {"Graphviz", "dot", runGraphviz},
                                 {"Markmap", "", nullptr}}; // drawn in-process by MarkmapBlock.cpp, never through make()
    return kinds[int(kind)];
}

/// What a renderer remembers between fences: the tool's resolved path (a
/// miss too) and every outcome by key. Shared by the closure and its jobs.
struct State {
    DiagramKind kind;
    QString setting;
    QMutex mutex;
    bool resolved = false;
    QString tool; ///< empty once resolved: not installed
    QHash<QByteArray, Made> memo;
    /// The texts a job is making now, each with the jobs for the same text that came meanwhile (the editor asks
    /// again at another width): they wait here, not on a thread, and are run when the text is made.
    QHash<QByteArray, QList<std::function<void()>>> making;
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

} // namespace

QImage rasteriseSVG(const QByteArray &svg, int width, qreal ratio, qreal *logicalScale)
{
    QSvgRenderer renderer(svg);
    QSizeF natural = renderer.isValid() ? QSizeF(renderer.defaultSize()) : QSizeF();
    if (natural.isEmpty() && renderer.isValid())
        natural = renderer.viewBoxF().size();
    if (natural.isEmpty())
        return {};
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
    if (logicalScale)
        *logicalScale = logicalWidth / natural.width();
    return image;
}

namespace {

/// The SVG at `width` logical pixels (never wider than it is, never
/// taller than kMaxPixels allows) and `ratio` device pixels to each.
Result rasterise(DiagramKind kind, const QByteArray &svg, const QString &source, int width, qreal ratio)
{
    QImage image = rasteriseSVG(svg, width, ratio);
    if (image.isNull())
        return sourceWithHint(source, QCoreApplication::translate("MermaidBlock", "%1: the tool's SVG could not be drawn").arg(QLatin1String(kindOf(kind).label)));
    return {false, QString(), std::move(image)};
}

/// The whole job: memo, then disk, then the tool. On a pool thread.
void make(const std::shared_ptr<State> &state, const QString &source, const QByteArray &config, int width, qreal ratio,
          const std::function<void(Result)> &deliver)
{
    const Kind &kind = kindOf(state->kind);
    const QString label = QLatin1String(kind.label);
    const QString cacheFile = diagramCachePath(state->kind, source, config);
    const QByteArray key = cacheFile.toUtf8(); // the hash is in the name
    Made made;
    bool have = false, owner = false;
    {
        QMutexLocker lock(&state->mutex);
        if (const auto it = state->memo.constFind(key); it != state->memo.cend()) {
            made = it.value();
            have = true;
        } else if (const auto making = state->making.find(key); making != state->making.end()) {
            making->append([=] { make(state, source, config, width, ratio, deliver); });
            return;
        } else {
            state->making.insert(key, {});
            owner = true;
        }
    }
    // Answers, then the jobs that waited for the same text (which find it in the memo).
    const auto finish = [&](Result result) {
        QList<std::function<void()>> followers;
        if (owner) {
            QMutexLocker lock(&state->mutex);
            followers = state->making.take(key);
        }
        deliver(std::move(result));
        for (const auto &follower : std::as_const(followers))
            follower();
    };
    if (!have) {
        made.svg = mermaidCachedSVG(cacheFile);
        have = !made.svg.isEmpty();
    }
    if (!have) {
        QString tool;
        {
            QMutexLocker lock(&state->mutex);
            if (!state->resolved) {
                state->tool = resolveTool(state->setting, QLatin1String(kind.defaultTool));
                state->resolved = true;
            }
            tool = state->tool;
        }
        if (tool.isEmpty()) {
            const QString name = state->setting.trimmed();
            finish(sourceWithHint(source, name.isEmpty() || name == QLatin1String(kind.defaultTool)
                                              ? QCoreApplication::translate("MermaidBlock", "%1: install %2 to render").arg(label, QLatin1String(kind.defaultTool))
                                              : QCoreApplication::translate("MermaidBlock", "%1: %2 was not found").arg(label, name)));
            return;
        }
        made = kind.run(tool, source, config);
        if (made.error.isEmpty())
            mermaidCacheStore(cacheFile, made.svg);
    }
    {
        QMutexLocker lock(&state->mutex);
        if (state->memo.size() >= 64)
            state->memo.clear(); // Limit: a flush, not an LRU; the disk copy answers the next ask
        state->memo.insert(key, made);
    }
    if (!made.error.isEmpty())
        return finish(sourceWithHint(source, QCoreApplication::translate("MermaidBlock", "%1: %2").arg(label, made.error)));
    finish(rasterise(state->kind, made.svg, source, width, ratio));
}

/// A pool per kind, two threads each, not the global pool's (parsing, search)
/// while a tool runs for seconds: a hung PlantUML or TeX (20 s, TeX's two
/// steps 40) leaves the other kinds untouched, and one hung fence leaves a
/// thread for the next of its kind. Few threads, as a JVM or TeX is heavy.
/// Built on the first diagram; the threads on the first job. Limit: two
/// hung fences of one kind still block it; per-key timeouts if that is felt.
QThreadPool *pool(DiagramKind kind)
{
    static QThreadPool pools[kKindCount];
    static const bool once = [] {
        for (QThreadPool &instance : pools)
            instance.setMaxThreadCount(2);
        return true;
    }();
    Q_UNUSED(once);
    return &pools[int(kind)];
}

} // namespace

MarkdownEditor::CodeBlockRenderer diagramRenderer(DiagramKind kind, const QString &tool, std::function<Theme()> theme)
{
    if (kind == DiagramKind::Markmap)
        return markmapRenderer(std::move(theme)); // no tool: its own renderer, in MarkmapBlock.cpp
    auto state = std::make_shared<State>();
    state->kind = kind;
    state->setting = tool;
    return [state, kind, theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                                   const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        // The UI thread's part: two reads, one queued job.
        const QByteArray config = diagramConfig(kind, theme());
        const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
        (void)QtConcurrent::run(pool(kind), [state, kind, source, config, width, ratio, deliver] {
            make(state, source, config, width, ratio, deliver);
            // Once per process and kind, after the first diagram is out, never at startup.
            static std::atomic_flag pruned[kKindCount];
            if (!pruned[int(kind)].test_and_set())
                pruneMermaidCache(diagramCacheDirectory(kind), kCacheDays, kCacheBytes);
        });
    };
}

MarkdownEditor::CodeBlockRenderer mermaidRenderer(const QString &tool, std::function<Theme()> theme)
{
    return diagramRenderer(DiagramKind::Mermaid, tool, std::move(theme));
}

MarkdownEditor::CodeBlockRenderer diagramCachedRenderer(DiagramKind kind, Theme theme)
{
    return [kind, theme = std::move(theme)](const QString &, const QString &source, int width, const QString &,
                                            const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        const QByteArray svg = diagramCachedSVG(kind, source, theme);
        deliver(svg.isEmpty() ? Result{true, QString(), QImage()} : rasterise(kind, svg, source, width, 2));
    };
}

MarkdownEditor::CodeBlockRenderer mermaidCachedRenderer(Theme theme)
{
    return diagramCachedRenderer(DiagramKind::Mermaid, std::move(theme));
}

} // namespace noteAppC
