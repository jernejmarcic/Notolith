// The PDF viewer module: what Notolith dlopens when a .pdf is opened in a tab or the
// "Extract annotations" command runs. Only this file knows it is a plugin; PdfView is plain Qt.
// PDF embeds in a note do not come through here: they use libnotolith-pdfpage.so by itself
// (libs/render/PdfPage.cpp), so a note with an embed never builds a tab's worth of code.
#include <QMessageBox>
#include <QObject>
#include <QtPlugin>

#include "ModuleHost.h"
#include "NotolithModule.h"
#include "PdfView.h"

// Global namespace on purpose: Q_PLUGIN_METADATA and moc want the plugin class plain, so every host type is spelled out.
class PdfModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    // No state of its own: a second window's attach() runs on this same object (NotolithModule.h).
    void attach(noteAppC::ModuleHost &host) override
    {
        host.registerView({QStringLiteral("pdf")}, [](const QString &, QWidget *parent) -> noteAppC::ModuleView * {
            return new noteAppC::PdfView(parent);
        });
        noteAppC::ModuleHost *const self = &host; // the host outlives its modules' commands
        host.registerCommand(QStringLiteral("pdf-viewer:extract-annotations"), QObject::tr("PDF: Extract annotations to a note"), [self] {
            const auto showStatus = [self](const QString &text) {
                if (self->showStatus)
                    self->showStatus(text, 3000);
            };
            auto *pdf = qobject_cast<noteAppC::PdfView *>(self->currentView ? self->currentView() : nullptr);
            if (!pdf) {
                showStatus(QObject::tr("Open a PDF first"));
                return;
            }
            if (self->flushSaves)
                self->flushSaves(); // the note it extracts into may be open, with a commit in flight
            QString error;
            if (!pdf->extractAnnotations(QString(), &error))
                QMessageBox::warning(pdf->window(), QObject::tr("Extract annotations"), error);
            else
                showStatus(QObject::tr("Annotations extracted"));
        });
    }
};

#include "PdfModule.moc"
