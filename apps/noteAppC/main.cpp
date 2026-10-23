#include <QApplication>
#include <cstdio>
#include <ctime>
#include <QCommandLineParser>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "MainWindow.h"
#include "PopupStyle.h"
#include "SingleInstance.h"

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

void setApplicationNames()
{
    // The product is Notolith; the organisation and application names stay
    // noteAppC, its code name, because they name the directories under
    // ~/.config, ~/.cache and ~/.local/share that hold everything a person
    // has set and saved - renaming them would strand all of it.
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOTEAPPC_VERSION));
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
            QStringLiteral("Notolith: Markdown notes and Obsidian vaults, native and fast"));
        parser.addOption(version);
        parser.addPositionalArgument(QStringLiteral("vault"),
                                     QStringLiteral("Vault directory to open."));
    }
};

QString handOverArgument(const QStringList &args)
{
    return args.isEmpty() ? QString() : QFileInfo(args.first()).absoluteFilePath();
}

// Help, version and the hand-over to a running Notolith need no window
// system, so they are answered by a plain QCoreApplication: no platform
// plugin, no platform theme, no style, no icon theme (85 ms of CPU for
// `--help` before, most of it QApplication's constructor). Anything this
// cannot settle - a Qt option such as -platform, a mistake in the
// arguments - falls through to the full start, which reports it as before.
bool answeredWithoutGUI(int argc, char *argv[])
{
    QCoreApplication core(argc, argv);
    setApplicationNames();
    Options options;
    if (!options.parser.parse(core.arguments()))
        return false;
    if (options.parser.isSet(options.help) && !core.arguments().contains(QStringLiteral("--help-all")))
        options.parser.showHelp(0);
    if (options.parser.isSet(options.version)) {
        std::printf("Notolith %s\n", NOTEAPPC_VERSION);
        return true;
    }
    // One Notolith per user: a launch while one runs (a note opened from the
    // file manager) gives it the path and ends here.
    noteAppC::SingleInstance instance(noteAppC::SingleInstance::defaultName());
    return instance.handOver(handOverArgument(options.parser.positionalArguments()));
}

} // namespace

int main(int argc, char *argv[])
{
    startLog("main");
    {
        // argc by value: QGuiApplication below strips the Qt options from
        // the real one, and this pass must not see that half done.
        int coreArgc = argc;
        if (answeredWithoutGUI(coreArgc, argv))
            return 0;
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
    noteAppC::SingleInstance instance(noteAppC::SingleInstance::defaultName());
    if (instance.handOver(handOverArgument(args)))
        return 0;

    noteAppC::MainWindow window;
    startLog("window");
    instance.serve(&window);
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
