#include <QApplication>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <QCommandLineParser>
#include <QEvent>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QIcon>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>

#include <optional>
#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef NOTEAPPC_STATIC_MODULES
#include <QtPlugin>
#endif

#include "ConfigFile.h"
#include "MainWindow.h"
#include "MessageLog.h"
#include "PopupStyle.h"
#include "SingleInstance.h"
#include "StorageMigration.h"

// The modules built in as Qt static plugins (NOTEAPPC_STATIC_MODULES; apps/noteAppC/CMakeLists.txt sets one define each):
// importing one is what makes the linker keep it, and ModuleRegistry::scan list it.
#ifdef NOTEAPPC_STATIC_INK
Q_IMPORT_PLUGIN(InkModule)
#endif
#ifdef NOTEAPPC_STATIC_HWFONT
Q_IMPORT_PLUGIN(HwfontModule)
#endif

namespace {

// NOTOLITH_START_LOG=<file> appends one "label microseconds" line per start
// milestone (CLOCK_MONOTONIC, so a driver that notes its own clock before the
// spawn gets spawn-to-first-paint); unset it costs one getenv. Measuring only.
void startLog(const char *label)
{
    static const QByteArray path = qgetenv("NOTOLITH_START_LOG");
    if (path.isEmpty())
        return;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (FILE *file = std::fopen(path.constData(), "a")) {
        std::fprintf(file, "%s %lld\n", label, qint64(now.tv_sec) * 1000000 + now.tv_nsec / 1000);
        std::fclose(file);
    }
}

// The first Paint event on the window, logged once.
struct FirstPaintLog : QObject {
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            startLog("painted");
            watched->removeEventFilter(this);
        }
        return false;
    }
};

// What migrateStorage() did, told once the log file is open.
QStringList migrationLines;
// What it could not move; also shown in the window.
QStringList migrationWarnings;

void setApplicationNames()
{
    // The product is Notolith. No directory is named after these: every path goes through appDir()
    // (libs/markdown/ConfigFile.h).
    QCoreApplication::setOrganizationName(QStringLiteral("notolith"));
    QCoreApplication::setApplicationName(QStringLiteral("notolith"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOTEAPPC_VERSION));
}

// Every Qt message is also appended to $XDG_STATE_HOME/notolith/notolith.log, one
// line each, so a bug report can carry what the terminal showed. Each line names
// its process: every vault runs as its own and they share the file (MessageLog).
// The earlier handler (Qt's own, when none was set) still runs, so terminal and journal output is unchanged.
noteAppC::MessageLog *messageLog = nullptr; // never freed: a message may come during static destruction
QtMessageHandler previousHandler = nullptr;

void logMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    static const char *const levels[] = {"debug", "warning", "critical", "fatal", "info"};
    messageLog->append(QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QStringLiteral(" [")
                       + QString::number(QCoreApplication::applicationPid()) + QStringLiteral("] ")
                       + QLatin1String(levels[type]) + QLatin1Char(' ')
                       + QString(message).replace(QLatin1Char('\n'), QLatin1Char(' ')) + QLatin1Char('\n'));
    if (previousHandler) {
        previousHandler(type, context, message);
        return;
    }
    // Qt's own handler is private API: its stderr line, and its abort for qFatal, are redone here.
    std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
    std::fflush(stderr);
    if (type == QtFatalMsg)
        std::abort();
}

void installMessageLog()
{
    const QString directory = noteAppC::appDir(noteAppC::XDGroot::State);
    if (directory.isEmpty() || !noteAppC::ensurePrivateDir(directory))
        return;
    // The lock goes in the runtime directory, which is gone with the session; without one, beside the log.
    const QString runtime = noteAppC::appDir(noteAppC::XDGroot::Runtime);
    if (!runtime.isEmpty())
        noteAppC::ensurePrivateDir(runtime);
    const QString lockPath = (runtime.isEmpty() ? directory : runtime) + QStringLiteral("/notolith.log.lock");
    messageLog = new noteAppC::MessageLog(directory + QStringLiteral("/notolith.log"), lockPath);
    previousHandler = qInstallMessageHandler(logMessage);
    for (const QString &line : std::as_const(migrationLines))
        qInfo().noquote() << line;
    for (const QString &line : std::as_const(migrationWarnings))
        qWarning().noquote() << line;
}

// A start that ends without a log (a hand-over) says on the terminal what the migration did.
void reportMigrationToTerminal()
{
    for (const QString &line : std::as_const(migrationLines))
        std::fprintf(stderr, "%s\n", qPrintable(line));
    for (const QString &line : std::as_const(migrationWarnings))
        std::fprintf(stderr, "%s\n", qPrintable(line));
}

struct Options {
    QCommandLineParser parser;
    QCommandLineOption help = parser.addHelpOption();
    // Not addVersionOption(): it prints applicationName(), the code name.
    QCommandLineOption version{{QStringLiteral("v"), QStringLiteral("version")},
                               QStringLiteral("Displays version information.")};

    Options()
    {
        parser.setApplicationDescription(
            QStringLiteral("Notolith: a Markdown note app for developers"));
        parser.addOption(version);
        parser.addPositionalArgument(QStringLiteral("vault"),
                                     QStringLiteral("Vault directory to open."));
    }
};

QString handOverArgument(const QStringList &args)
{
    return args.isEmpty() ? QString() : QFileInfo(args.first()).absoluteFilePath();
}

constexpr int kOlderWindowOpen = 3;

// Moves the files of a build from before the XDG layout to where they belong, once, when a start follows
// (noteAppC::settleStorage decides, with the windows that are open in view). Before anything reads the
// settings. Not for --help and --version. Once per process: the pass before the window system and the one
// after it would otherwise both walk the old directories and say the same things twice.
// Returns the exit code when the process is done, nothing when it carries on.
std::optional<int> settleStorage(const QString &argument)
{
    static bool settled = false;
    if (settled)
        return std::nullopt;
    settled = true;
    switch (noteAppC::settleStorage(argument, &migrationLines, &migrationWarnings)) {
    case noteAppC::StorageSettled::HandedOver:
        return 0;
    case noteAppC::StorageSettled::OlderWindowOpen:
        return kOlderWindowOpen;
    case noteAppC::StorageSettled::Carry:
        break;
    }
    return std::nullopt;
}

// Said when settleStorage() refused: on the terminal, and in a box when there is a display to show it on.
void refuseWhileOlderWindowOpen(int argc, char *argv[])
{
    const QString text = QCoreApplication::translate(
        "Notolith", "An older version of Notolith is still open. Close it, then start Notolith again: its files "
                    "are moved to their new places when it starts.");
    std::fprintf(stderr, "Notolith: %s\n", qPrintable(text));
    if (qApp) {
        QMessageBox::warning(nullptr, QStringLiteral("Notolith"), text);
        return;
    }
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("DISPLAY"))
        return;
    QApplication app(argc, argv);
    QMessageBox::warning(nullptr, QStringLiteral("Notolith"), text);
}

// Help, version and the hand-over to a running Notolith need no window
// system, so they are answered by a plain QCoreApplication: no platform
// plugin, no platform theme, no style, no icon theme (85 ms of CPU for
// `--help` before, most of it QApplication's constructor). Anything this
// cannot settle - a Qt option such as -platform, a mistake in the
// arguments - falls through to the full start, which reports it as before.
std::optional<int> answeredWithoutGUI(int argc, char *argv[])
{
    QCoreApplication core(argc, argv);
    setApplicationNames();
    Options options;
    if (!options.parser.parse(core.arguments()))
        return std::nullopt;
    if (options.parser.isSet(options.help) && !core.arguments().contains(QStringLiteral("--help-all")))
        options.parser.showHelp(0);
    if (options.parser.isSet(options.version)) {
        std::printf("Notolith %s\n", NOTEAPPC_VERSION);
        return 0;
    }
    // One Notolith per vault: a launch while one has that vault open (a note
    // opened from the file manager) gives it the path and ends here.
    const QString argument = handOverArgument(options.parser.positionalArguments());
    if (const std::optional<int> done = settleStorage(argument))
        return done;
    noteAppC::SingleInstance instance(noteAppC::SingleInstance::launchFor(argument));
    if (instance.handOver(argument))
        return 0;
    return std::nullopt;
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef __GLIBC__
    // glibc raises its mmap and trim thresholds by itself the first time a large buffer is freed, so whether a
    // 4-8 MB buffer (a window's backing store, a page raster) is reused or mapped and faulted in again on every
    // repaint hangs on what happened to be freed earlier in the process. Taking the sharing libraries out of the
    // start moved it: +1100 page faults and +0.6 ms on every PDF view open. Pinned instead: buffers up to 16 MiB
    // come from the heap, which gives memory back only when more than 16 MiB is free at its top (the 8 s trim
    // below returns the rest). tools/bench/Bench.cpp sets the same two so its rows measure this process.
    mallopt(M_MMAP_THRESHOLD, 16 << 20);
    mallopt(M_TRIM_THRESHOLD, 16 << 20);
#endif
    startLog("main");
    {
        // argc by value: QGuiApplication below strips the Qt options from
        // the real one, and this pass must not see that half done.
        int coreArgc = argc;
        if (const std::optional<int> done = answeredWithoutGUI(coreArgc, argv)) {
            reportMigrationToTerminal();
            if (*done == kOlderWindowOpen) {
                refuseWhileOlderWindowOpen(argc, argv);
                return 1;
            }
            return *done;
        }
    }
    // Fusion, so the window looks the same whatever desktop theme is set;
    // everything visible on top of it comes from Theme's tokens. Set before
    // the QApplication exists, as Qt advises for setStyle(QStyle *), so the
    // style the desktop names is never loaded just to be replaced. The
    // constructor would still load QT_STYLE_OVERRIDE's plugin (Breeze, on a
    // KDE desktop) and install it over ours, so the variable is hidden from it
    // and put back for the programs this one starts.
    const QByteArray styleOverride = qgetenv("QT_STYLE_OVERRIDE");
    qunsetenv("QT_STYLE_OVERRIDE");
    QApplication::setStyle(new noteAppC::PopupStyle);
    QApplication app(argc, argv);
    if (!styleOverride.isEmpty())
        qputenv("QT_STYLE_OVERRIDE", styleOverride);
    startLog("app");
    setApplicationNames();
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Notolith"));
    // The installed icon by name when the desktop has it, the built-in copy
    // otherwise; the desktop file name lets a Wayland compositor match the window.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.jernejmarcic.Notolith"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.jernejmarcic.Notolith"), QIcon(QStringLiteral(":/icons/noteAppC.svg"))));

    Options options;
    options.parser.process(app);
    const QStringList args = options.parser.positionalArguments();
    const QString argument = handOverArgument(args);
    // The first pass settled this when it could parse the arguments; a Qt option it could not is settled here.
    if (const std::optional<int> done = settleStorage(argument)) {
        reportMigrationToTerminal();
        if (*done == kOlderWindowOpen) {
            refuseWhileOlderWindowOpen(argc, argv);
            return 1;
        }
        return *done;
    }
    noteAppC::SingleInstance instance(noteAppC::SingleInstance::launchFor(argument));
    if (instance.handOver(argument))
        return 0;
    // After the hand-over: a launch that only passes a path on must not
    // rotate the log of the Notolith that is running.
    installMessageLog();

    noteAppC::MainWindow window;
    startLog("window");
    instance.serve(&window);
    window.setSingleInstance(&instance); // its vault, when it changes, moves the name it serves
    const noteAppC::LaunchTarget target =
        args.isEmpty() ? noteAppC::LaunchTarget{} : noteAppC::resolveLaunchTarget(args.first());

    // Show first, open second. The vault is read once the empty shell has been
    // painted, so the window is on screen in a fraction of the time and fills
    // in, rather than the user waiting on nothing for the whole load.
    FirstPaintLog paintLog;
    if (qEnvironmentVariableIsSet("NOTOLITH_START_LOG"))
        window.installEventFilter(&paintLog);
    window.show();
    startLog("shown");
    if (!migrationWarnings.isEmpty())
        window.statusBar()->showMessage(
            QCoreApplication::translate("Notolith", "Some files of the previous version could not be moved; see the log."),
            15000);
    window.openWhenShown(target);

#ifdef __GLIBC__
    // The cold index and the first notes leave a third of the heap free but
    // resident, scattered through 24 worker arenas that glibc never hands back
    // on its own: 460 MB resident at 20 000 notes and a 1 MB note, 347 MB after
    // this (5 ms on a worker thread; once, so an idle window still wakes for
    // the caret alone). Capping the arenas (M_ARENA_MAX=2) got 415 MB but made
    // the cold index 28% slower. A big vault that is still indexing at 8 s
    // keeps what it has; the ceiling is one pass, not a policy.
    QTimer::singleShot(8000, &app, [] { std::thread([] { malloc_trim(0); }).detach(); });
    // The same for a big note opened later, which the timer above misses: the
    // parse leaves 100 MB of its temporaries free but resident in a pool
    // thread's arena, and the trim returns them once per such note (5 ms). A
    // quarter of a second on: run beside the landing's own paint it cost that
    // frame 0.7 ms (6.6 -> 7.3 ms), the contention of two threads and the
    // page freeing.
    noteAppC::MarkdownEditor::setBigParseLandedHook([] {
        QTimer::singleShot(250, qApp, [] { std::thread([] { malloc_trim(0); }).detach(); });
    });
#endif

    return app.exec();
}
