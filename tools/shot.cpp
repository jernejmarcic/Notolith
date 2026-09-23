// Renders the window to a PNG on the offscreen platform. Used to review the
// look without a display, and the harness the Phase 13 golden-image tests grow
// out of.
//
//   noteAppC_shot <vault> <note> <out.png> [dark|light] [width height]
//   noteAppC_shot - <file>  <out.png> [dark|light] [w h] --single
//   ... [--command <id> [--type <text>]]

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QImage>
#include <QLineEdit>
#include <QStyleFactory>
#include <QTimer>

#include "MainWindow.h"
#include "Settings.h"

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
    // A command-line tool talks on stderr. Without this Qt 6 sends qWarning
    // to the systemd journal whenever stderr is not a terminal - piped, run
    // from a script, run by an agent - and every error the tool reports simply
    // vanishes.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING"))
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
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

    // `--single <file>`: the mode a loose Markdown file opens in, which has no
    // vault and so cannot be reached through the vault argument.
    if (args.contains(QLatin1String("--single"))) {
        const QFileInfo note(args.at(2));
        if (!note.isFile()) {
            qWarning("%s is not a file", qPrintable(args.at(2)));
            return 2;
        }
        window.openTarget(noteAppC::resolveLaunchTarget(note.absoluteFilePath()));
        window.setThemeMode(noteAppC::Theme::modeFromName(args.value(4)));
        if (sourceMode)
            window.toggleEditingMode();
        window.show();
        int status = 0;
        QTimer::singleShot(600, &app, [&] {
            if (!window.grab().toImage().save(args.at(3))) {
                qWarning("could not write %s", qPrintable(args.at(3)));
                status = 1;
            }
            app.quit();
        });
        app.exec();
        return status;
    }

    // Refuse a vault or note that is not there before touching either. Opening
    // a missing note raises a modal "could not read" box, and on the offscreen
    // platform nobody can dismiss it, so the tool used to hang forever instead
    // of failing - which looked, the one time it happened, exactly like a hang
    // in whatever code had just been changed.
    if (!QFileInfo(args.at(1)).isDir()) {
        qWarning("%s is not a directory", qPrintable(args.at(1)));
        return 2;
    }
    if (args.at(2) != QLatin1String("-") && !QFileInfo(args.at(2)).isFile()) {
        qWarning("%s is not a file", qPrintable(args.at(2)));
        return 2;
    }

    // Absolute, always: a relative note path makes Vault::read fail, and the
    // error box it raises blocks forever on the offscreen platform.
    window.openVault(QFileInfo(args.at(1)).absoluteFilePath());
    if (args.at(2) != QLatin1String("-"))
        window.openNote(QFileInfo(args.at(2)).absoluteFilePath()); // checked above
    // After opening, because opening a vault applies that vault's saved
    // scheme and would otherwise overwrite the mode asked for here.
    window.setThemeMode(noteAppC::Theme::modeFromName(args.value(4)));
    if (sourceMode)
        window.toggleEditingMode();
    window.show();

    // `--command <id>` triggers a window command (its Obsidian command id)
    // once the vault is indexed, and `--type <text>` then types into whatever
    // it focused: enough to show a pane or a search the way a user reaches it.
    const auto flag = [&args](const char *name) {
        const qsizetype at = args.indexOf(QLatin1String(name));
        return at >= 0 ? args.value(at + 1) : QString();
    };
    const QString command = flag("--command");
    const QString typed = flag("--type");

    int status = 0;
    const auto capture = [&] {
        const QImage shot = window.grab().toImage();
        if (!shot.save(args.at(3))) {
            qWarning("could not write %s", qPrintable(args.at(3)));
            status = 1;
        }
        app.quit();
    };
    // One turn of the event loop so QFileSystemModel's worker fills the tree.
    QTimer::singleShot(600, &app, [&] {
        if (command.isEmpty())
            return capture();
        auto *action = window.findChild<QAction *>(command);
        if (!action) {
            qWarning("no command %s", qPrintable(command));
            status = 1;
            return app.quit();
        }
        action->trigger();
        if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget()); edit && !typed.isEmpty())
            edit->setText(typed);
        QTimer::singleShot(1200, &app, capture); // a debounced search, a settling graph
    });
    app.exec();
    return status;
}
