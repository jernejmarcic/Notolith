// Renders the window to a PNG on the offscreen platform. Used to review the
// look without a display, and the harness the Phase 13 golden-image tests grow
// out of.
//
//   noteAppC_shot <vault> <note> <out.png> [dark|light] [width height]
//   noteAppC_shot - <file>  <out.png> [dark|light] [w h] --single
//   ... [--command <id> [--type <text>]] [--click <button object name>]...
//   ... [--popup <QMenu object name>] [--tooltip <text>]   a menu / tooltip over the window

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QToolButton>
#include <QToolTip>
#include <QTimer>

#include "MarkdownEditor.h"
#include "MainWindow.h"
#include "PopupStyle.h"
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
    QApplication::setStyle(new noteAppC::PopupStyle);
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
            window.toggleSourceMode();
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
        window.toggleSourceMode();
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
    // `--click <objectName>` (repeatable) presses that button once the command
    // has run: the way to open a panel that only a click reaches.
    const auto clickButtons = [&] {
        for (qsizetype at = args.indexOf(QLatin1String("--click")); at >= 0 && at + 1 < args.size();
             at = args.indexOf(QLatin1String("--click"), at + 1)) {
            // The visible one: a window can hold several of the same name (the
            // graph and the local graph both have a controls button).
            QAbstractButton *button = nullptr;
            for (auto *candidate : window.findChildren<QAbstractButton *>(args.at(at + 1))) {
                if (!button || candidate->isVisible())
                    button = candidate;
                if (candidate->isVisible())
                    break;
            }
            if (!button) {
                qWarning("no button %s", qPrintable(args.at(at + 1)));
                status = 1;
                continue;
            }
            button->click();
        }
    };
    const auto capture = [&] {
        QImage shot = window.grab().toImage();
        // The window system's own composite, which includes translucent popups
        // the way a user sees them: the one thing a widget grab cannot show.
        if (qEnvironmentVariableIsSet("NOTEAPPC_SHOT_SCREEN")) {
            shot = QGuiApplication::primaryScreen()->grabWindow(0).toImage();
            if (!shot.save(args.at(3)))
                status = 1;
            return app.quit();
        }
        // A popup - the switcher, the command palette - is a window of its
        // own, which grabbing the main window leaves out.
        QPainter painter(&shot);
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top != &window && top->isVisible() && (top->windowType() == Qt::Popup || top->windowType() == Qt::ToolTip))
                painter.drawPixmap(window.mapFromGlobal(top->mapToGlobal(QPoint(0, 0))), top->grab());
        }
        painter.end();
        if (!shot.save(args.at(3))) {
            qWarning("could not write %s", qPrintable(args.at(3)));
            status = 1;
        }
        app.quit();
    };
    // One turn of the event loop so QFileSystemModel's worker fills the tree.
    QTimer::singleShot(600, &app, [&] {
        // `--popup <name>` opens the menu of that name above its button, and
        // `--tooltip <text>` shows a tooltip there: separate windows, grabbed
        // with the rest in capture().
        if (auto *menu = window.findChild<QMenu *>(flag("--popup")); menu && !flag("--popup").isEmpty()) {
            const auto *button = qobject_cast<QWidget *>(menu->parent());
            menu->popup(button->mapToGlobal(QPoint(button->width() - menu->sizeHint().width(),
                                                    -menu->sizeHint().height() - 4)));
        }
        if (!flag("--tooltip").isEmpty()) {
            auto *button = window.findChild<QToolButton *>(QStringLiteral("backlinkStatus"));
            QToolTip::showText(button->mapToGlobal(QPoint(0, -36)), flag("--tooltip"), button);
        }
        // `--keys <text>` types into the focused widget, for a suggester popup.
        if (const QString keys = flag("--keys"); !keys.isEmpty()) {
            for (const QChar c : keys) {
                QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                QCoreApplication::sendEvent(QApplication::focusWidget(), &press);
            }
            return QTimer::singleShot(600, &app, capture);
        }
        if (command.isEmpty()) {
            if (args.contains(QLatin1String("--click"))) {
                clickButtons();
                return QTimer::singleShot(600, &app, capture);
            }
            if (flag("--popup").isEmpty() && flag("--tooltip").isEmpty())
                return capture();
            return QTimer::singleShot(400, &app, capture); // let the popup map
        }
        auto *action = window.findChild<QAction *>(command);
        if (!action) {
            // An editor command ("noteAppC:format-all-tables") runs in the
            // note on screen.
            for (auto *editor : window.findChildren<noteAppC::MarkdownEditor *>()) {
                if (editor->isVisible() && editor->runCommand(command)) {
                    QTimer::singleShot(600, &app, capture);
                    return;
                }
            }
            qWarning("no command %s", qPrintable(command));
            status = 1;
            return app.quit();
        }
        action->trigger();
        if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget()); edit && !typed.isEmpty())
            edit->setText(typed);
        // After the command's own work has had a moment to land: a graph's
        // controls button exists as soon as it is opened, a pane's content
        // not until its worker is done.
        QTimer::singleShot(1200, &app, [&] {
            clickButtons();
            QTimer::singleShot(400, &app, capture); // a debounced search, a settling graph
        });
    });
    app.exec();
    return status;
}
