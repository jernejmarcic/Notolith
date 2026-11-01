// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// libFuzzer target: the JSON a vault keeps in `.obsidian/`, which a vault's
// author controls. The input is written as every file the importers read,
// then each importer runs over it. The vault is one temporary folder, made
// once; its files are rewritten for every input.

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "Bookmarks.h"
#include "DailyNotes.h"
#include "Hotkeys.h"
#include "MomentFormat.h"
#include "Settings.h"
#include "Templates.h"
#include "Workspaces.h"

using namespace noteAppC;

namespace {

QTemporaryDir *vault = nullptr;

const char *const kFiles[] = {"app.json",     "appearance.json", "core-plugins.json", "hotkeys.json",
                              "workspace.json", "workspaces.json", "bookmarks.json",    "daily-notes.json",
                              "templates.json"};

} // namespace

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    static QCoreApplication app(*argc, *argv);
    vault = new QTemporaryDir;
    QDir(vault->path()).mkpath(QStringLiteral(".obsidian"));
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const QDir config(vault->filePath(QStringLiteral(".obsidian")));
    for (const char *name : kFiles) {
        QFile file(config.filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(bytes);
    }
    const QString root = vault->path();

    Settings::fromObsidianConfig(root);
    Hotkeys::load(root, true);
    Workspaces::current(root);
    Workspaces::named(root);
    Bookmarks bookmarks(root);
    bookmarks.load();

    const DailyNotes::Config daily = DailyNotes::readConfig(root);
    const DailyNotes notes(root, daily);
    notes.pathFor(QDate(2026, 11, 1));
    const Templates::Config templates = Templates::readConfig(root);
    const QString text = QString::fromUtf8(bytes);
    const QDateTime when(QDate(2026, 11, 1), QTime(12, 30));
    Templates::renderText(text, QStringLiteral("Title"), when, templates.dateFormat, templates.timeFormat);
    MomentFormat::format(when, text);
    return 0;
}
