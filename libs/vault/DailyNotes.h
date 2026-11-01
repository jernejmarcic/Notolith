// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QDate>
#include <QString>

namespace noteAppC {

/// `DailyNotes::Config`'s data, kept out of the class body: a default
/// argument of `DailyNotes`'s own constructor needs this type's default
/// member initializers already resolved, which GCC cannot do for a struct
/// nested in the very class whose constructor is being declared.
struct DailyNotesConfig {
    QString folder;                               ///< vault-relative; empty means the vault root
    QString format = QStringLiteral("YYYY-MM-DD"); ///< Obsidian's own default
    /// Vault-relative, without ".md" - Obsidian stores it the way it
    /// stores a link target. Empty means no template.
    QString templatePath;
};

/// Obsidian's core Daily notes plugin: today's note from a folder, a
/// moment.js date format and a template.
class DailyNotes {
public:
    using Config = DailyNotesConfig;

    /// Reads `.obsidian/daily-notes.json`'s folder/format/template when the
    /// vault has one; Obsidian's own defaults otherwise.
    static Config readConfig(const QString &vaultRoot);

    explicit DailyNotes(const QString &vaultRoot, Config config = {});

    /// The absolute path `date`'s note has, whether or not it exists yet.
    QString pathFor(const QDate &date) const;

    /// Creates the note from the configured template if it is not already
    /// there - through Vault::write, so line endings and the rest of the
    /// round-trip rules apply to it like any other note - and returns the
    /// path to open either way. Empty with `error` set on a write failure.
    QString ensureNote(const QDate &date, QString *error = nullptr) const;

    /// The nearest existing daily note strictly before/after `date`, found by
    /// walking calendar days and checking whether that day's path exists,
    /// rather than parsing filenames back into dates - an arbitrary format
    /// string is not guaranteed to round-trip. Empty when none turns up
    /// within about ten years.
    QString previousNote(const QDate &date) const;
    QString nextNote(const QDate &date) const;

private:
    QString m_vaultRoot;
    Config m_config;
};

} // namespace noteAppC
