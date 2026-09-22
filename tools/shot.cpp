// Renders the window to a PNG on the offscreen platform. Used to review the
// look without a display, and the harness the Phase 13 golden-image tests grow
// out of.
//
//   noteAppC_shot <vault> <note> <out.png> [dark|light] [width height]

#include <QApplication>
#include <QImage>
#include <QStyleFactory>
#include <QTimer>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
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
    if (args.value(4) == QLatin1String("light"))
        window.setThemeMode(noteAppC::Theme::Mode::Light);
    const bool sourceMode = args.contains(QLatin1String("--source"));
    window.resize(size);
    window.openVault(args.at(1));
    if (args.at(2) != QLatin1String("-"))
        window.openNote(args.at(2));
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
