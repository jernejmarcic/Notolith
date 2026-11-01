// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// `Templates::Config`'s data, kept out of the class body: a default argument
/// of `Templates`'s own constructor needs this type's default member
/// initializers already resolved, which GCC cannot do for a struct nested in
/// the very class whose constructor is being declared.
struct TemplatesConfig {
    QString folder;                                   ///< vault-relative; empty means the vault root
    QString dateFormat = QStringLiteral("YYYY-MM-DD"); ///< moment.js pattern, Obsidian's default
    QString timeFormat = QStringLiteral("HH:mm");
};

/// Obsidian's core Templates plugin: a folder of notes whose `{{...}}` tags
/// are filled in when one is inserted into another.
class Templates {
public:
    using Config = TemplatesConfig;

    /// Reads `.obsidian/templates.json`'s folder/dateFormat/timeFormat when
    /// the vault has one; Obsidian's own defaults otherwise.
    static Config readConfig(const QString &vaultRoot);

    /// `vaultRoot` may be empty for a loose file opened with no vault at all:
    /// then the folder is always Settings::templatesDirectory(), noteAppC's
    /// own template stash for exactly that case - never created just by
    /// listing it, so a stray file leaves nothing behind.
    explicit Templates(const QString &vaultRoot, Config config = {});

    /// Every `.md` template under the configured folder, as paths relative to
    /// it with the extension dropped - what the picker lists. Empty when the
    /// folder does not exist; this never creates it.
    QStringList list() const;

    /// Renders the template `list()` names `templateName`. `noteTitle` fills
    /// {{title}} - the note the template is being inserted into, not the
    /// template itself. False with `error` set if the file cannot be read.
    bool render(const QString &templateName, const QString &noteTitle, QString *out,
               QString *error = nullptr) const;

    /// The substitution alone, shared with DailyNotes so a daily note's own
    /// template is filled in exactly the same way: {{title}}, {{date}},
    /// {{time}}, {{date:FORMAT}}, {{time:FORMAT}}. `dateFormat`/`timeFormat`
    /// apply to a bare {{date}}/{{time}} with no format of its own.
    static QString renderText(const QString &content, const QString &noteTitle,
                              const QDateTime &when, const QString &dateFormat,
                              const QString &timeFormat);

private:
    QString folderAbsolute() const;
    bool inVault(const QString &path) const;

    QString m_vaultRoot;
    Config m_config;
};

} // namespace noteAppC
