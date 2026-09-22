// Renders the window to a PNG on the offscreen platform. Used to review the
// look without a display, and the harness the Phase 13 golden-image tests grow
// out of.
//
//   noteAppC_shot <vault> <note> <out.png> [dark|light] [width height]

#include <QApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QImage>
#include <QStyleFactory>
#include <QTimer>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // A screenshot must not touch the real ~/.config/noteAppC: the window
    // saves settings when it closes, and a review run would quietly rewrite
    // whoever is looking at it. Point the whole config tree at a temporary
    // directory unless the caller has already redirected it.
    QTemporaryDir configHome;
    if (configHome.isValid() && !qEnvironmentVariableIsSet("NOTEAPPC_SHOT_KEEP_CONFIG"))
        qputenv("XDG_CONFIG_HOME", configHome.path().toLocal8Bit());
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-shot"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-shot"));

    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 4) {
        qWarning("usage: %s <vault> <note> <out.png> [dark|light] [width height]",
                 qPrintable(args.value(0)));
        return 2;
    }

    const QSize size(args.value(5).toInt() > 0 ? args.value(5).toInt() : 1180,
                     args.value(6).toInt() > 0 ? args.value(6).toInt() : 760);

    noteAppC::MainWindow window;
    const bool sourceMode = args.contains(QLatin1String("--source"));
    window.resize(size);
    // Absolute, always: a relative note path makes Vault::read fail, and the
    // error box it raises blocks forever on the offscreen platform.
    window.openVault(QFileInfo(args.at(1)).absoluteFilePath());
    if (args.at(2) != QLatin1String("-")) {
        const QFileInfo note(args.at(2));
        if (!note.isFile()) {
            qWarning("%s is not a file", qPrintable(args.at(2)));
            return 2;
        }
        window.openNote(note.absoluteFilePath());
    }
    // After opening, because opening a vault applies that vault's saved
    // scheme and would otherwise overwrite the mode asked for here.
    window.setThemeMode(args.value(4) == QLatin1String("light") ? noteAppC::Theme::Mode::Light
                                                                : noteAppC::Theme::Mode::Dark);
    if (sourceMode)
        window.toggleEditingMode();
    window.show();

    int status = 0;
    // One turn of the event loop so QFileSystemModel's worker fills the tree.
    QTimer::singleShot(600, &app, [&] {
        const QImage shot = window.grab().toImage();
        if (!shot.save(args.at(3))) {
            qWarning("could not write %s", qPrintable(args.at(3)));
            status = 1;
        }
        app.quit();
    });
    app.exec();
    return status;
}
