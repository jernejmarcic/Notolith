// The handwritten-note module (spec section 4): what Notolith dlopens when a
// .ink file is opened or the "New handwritten note" command runs. Only this
// file knows it is a plugin; InkView and the core are plain Qt.
#include <QDir>
#include <QObject>
#include <QtPlugin>

#include "InkView.h"
#include "ModuleHost.h"
#include "NotolithModule.h"
#include "Vault.h"
#include "core/InkFormat.h"

// Global namespace on purpose: Q_PLUGIN_METADATA and moc want the plugin class
// plain, so every host type is spelled out.
class InkModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    // No state of its own: a second window's attach() runs on this same object (NotolithModule.h),
    // so everything the registrations need is captured from the host they are registered on.
    void attach(noteAppC::ModuleHost &host) override
    {
        const noteAppC::Settings *settings = host.settings;
        host.registerView({QStringLiteral("ink")}, [settings](const QString &, QWidget *parent) -> noteAppC::ModuleView * {
            return new noteAppC::ink::InkView(settings, parent);
        });
        host.registerSetting({QStringLiteral("ink.drawWithMouse"), false, QObject::tr("Draw with the mouse"),
                              QObject::tr("Off, the mouse only scrolls and selects; a pen always draws."), {}, 0, 0});
        host.registerSetting({QStringLiteral("ink.rawInput"), false, QObject::tr("Raw pen input"),
                              QObject::tr("Skip the stroke modeler: every tablet sample becomes a point. For comparing."), {}, 0, 0});
        host.registerSetting({QStringLiteral("ink.tileCacheMB"), 64, QObject::tr("Handwriting tile cache (MB)"),
                              QObject::tr("Memory for the rendered tiles of a handwritten note; the least recently shown are dropped past it."),
                              {}, 8, 1024, 8, 0});
        const auto currentFolder = host.currentFolder;
        const auto showStatus = host.showStatus;
        const auto openFile = host.openFile;
        host.registerCommand(QStringLiteral("ink:new"), QObject::tr("New handwritten note"), [currentFolder, showStatus, openFile] {
            const QString folder = currentFolder ? currentFolder() : QString(); // beside the active note, else the vault root
            if (folder.isEmpty())
                return;
            const QDir root(folder);
            QString name = QStringLiteral("Handwritten note.ink");
            for (int n = 2; root.exists(name); ++n)
                name = QStringLiteral("Handwritten note %1.ink").arg(n);
            noteAppC::ink::InkDocument empty;
            QString error;
            if (!noteAppC::Vault::writeBytes(root.filePath(name), noteAppC::ink::writeInk(empty), &error)) {
                if (showStatus)
                    showStatus(QObject::tr("Could not create %1: %2").arg(name, error), 6000);
                return;
            }
            if (openFile)
                openFile(root.filePath(name));
        });
    }
};

#include "InkModule.moc"
