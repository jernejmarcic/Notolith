// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// The handwriting-font studio module (spec section 9): what Notolith dlopens when the
// "Open glyph studio" command runs. Only this file knows it is a plugin; GlyphStudio is plain Qt.
#include <QDir>
#include <QObject>
#include <QtPlugin>

#include "GlyphStudio.h"
#include "ModuleHost.h"
#include "NotolithModule.h"
#include "Settings.h"
#include "Vault.h"

// Global namespace on purpose: Q_PLUGIN_METADATA and moc want the plugin class plain, so every host type is spelled out.
class HwfontModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    // No state of its own: a second window's attach() runs on this same object (NotolithModule.h).
    void attach(noteAppC::ModuleHost &host) override
    {
        // SettingRow has no free-text kind, so the font is a choice among the fonts already in the vault
        // (the folders of Handwriting/) and the default name; a new font is a new folder.
        const QString defaultFont = QStringLiteral("My handwriting");
        QList<QPair<QString, QVariant>> fonts{{defaultFont, defaultFont}};
        if (host.vault && host.vault->isValid()) {
            const QStringList existing = QDir(host.vault->root() + QStringLiteral("/Handwriting")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &name : existing) {
                if (name != defaultFont)
                    fonts.append({name, name});
            }
        }
        host.registerSetting({QStringLiteral("hwfont.fontName"), defaultFont, QObject::tr("Handwriting font"),
                              QObject::tr("The font the glyph studio writes into: the folder Handwriting/<font>/ in the vault."), fonts, 0, 0});
        host.registerSetting({QStringLiteral("hwfont.samplesPerLetter"), 3, QObject::tr("Samples per letter"),
                              QObject::tr("How many hand-written variants the studio asks for each letter."), {}, 1, 10});
        host.registerSetting({QStringLiteral("hwfont.samplesPerPunctuation"), 1, QObject::tr("Samples per digit and punctuation mark"),
                              QObject::tr("How many variants the studio asks for each digit and punctuation mark."), {}, 1, 10});
        noteAppC::ModuleHost *const self = &host; // the host outlives its modules' commands
        host.registerCommand(QStringLiteral("handwriting-font:studio"), QObject::tr("Open glyph studio"), [self] {
            if (!self->showTab)
                return;
            if (!self->vault || !self->vault->isValid()) {
                if (self->showStatus)
                    self->showStatus(QObject::tr("Open a vault first: the studio saves its samples there."), 6000);
                return;
            }
            const QString font = self->settings ? self->settings->text(QStringLiteral("hwfont.fontName")) : QString();
            self->showTab(new noteAppC::hwfont::GlyphStudio(*self, font.isEmpty() ? QStringLiteral("My handwriting") : font),
                          QObject::tr("Glyph studio"));
        });
    }
};

#include "HwfontModule.moc"
