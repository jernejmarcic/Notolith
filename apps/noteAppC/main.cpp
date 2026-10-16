#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

#include "MainWindow.h"
#include "PopupStyle.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Fusion, so the window looks the same whatever desktop theme is set;
    // everything visible on top of it comes from Theme's tokens.
    QApplication::setStyle(new noteAppC::PopupStyle);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOTEAPPC_VERSION));
    // The installed icon by name when the desktop has it, the built-in copy
    // otherwise; the desktop file name lets a Wayland compositor match the window.
    QGuiApplication::setDesktopFileName(QStringLiteral("noteAppC"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("noteAppC"), QIcon(QStringLiteral(":/icons/noteAppC.svg"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("A Linux-native Obsidian alternative in C++"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("vault"),
                                 QStringLiteral("Vault directory to open."));
    parser.process(app);

    noteAppC::MainWindow window;
    const QStringList args = parser.positionalArguments();
    const noteAppC::LaunchTarget target =
        args.isEmpty() ? noteAppC::LaunchTarget{} : noteAppC::resolveLaunchTarget(args.first());

    // Show first, open second. The vault is read once the empty shell has been
    // painted, so the window is on screen in a fraction of the time and fills
    // in, rather than the user waiting on nothing for the whole load.
    window.show();
    window.openWhenShown(target);

    return app.exec();
}
