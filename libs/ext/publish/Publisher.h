// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <functional>
#include <optional>

#include <QString>

#include "Parser.h"
#include "Theme.h"

namespace noteAppC {
class Vault;
class Index;
} // namespace noteAppC

namespace noteAppC::publish {

/// What one publish() call is asked to do.
struct Options {
    /// Where the site is written. Never the vault root itself - see
    /// Publisher::publish() for the exact refusal rule and why a dedicated
    /// subfolder of the vault is fine.
    QString outputDir;
    /// True: only notes with `publish: true` go out. False (the default):
    /// every note goes out except one with `publish: false` - Obsidian
    /// Publish's own rule, where inclusion is normally decided by which
    /// notes are checked in the app and `publish: false` is the override; a
    /// library with no such UI needs a rule for "everything", and this is
    /// it.
    bool onlyMarked = false;
    /// Vault-relative path of the note that becomes index.html. Empty: a
    /// generated list of every published note by folder.
    QString homeNote;
    QString siteName = QStringLiteral("Notes");
    enum class ThemeMode { Dark, Light, Both };
    /// "Both" ships one stylesheet with light tokens under
    /// `@media (prefers-color-scheme: light)`, so the reader's own OS/browser
    /// preference decides - no toggle, no JavaScript needed for it.
    ThemeMode theme = ThemeMode::Both;
    /// The schemes the code colours come from (`--code-*`, a scheme's
    /// `syntax`), one per mode; unset is the built-in scheme, which names none
    /// and so leaves the highlighter's own colours.
    std::optional<noteAppC::Theme> codeThemeDark;
    std::optional<noteAppC::Theme> codeThemeLight;
    /// What the editor reads a note with, so a published note is the one on
    /// screen: only taskStates changes what the page says.
    noteAppC::ParseOptions parse;
    /// The markmap plugin: on, a ```markmap fence goes out as the map, drawn
    /// from its text; off, as the plain listing every fence is.
    bool mindMaps = true;
};

/// What one publish() call did, for the caller to report.
struct Counts {
    int pages = 0;       ///< note pages written or confirmed unchanged
    int attachments = 0; ///< images/audio/video/PDFs written or confirmed unchanged
    int skipped = 0;     ///< vault notes that were not published (privacy filter)
    int unchanged = 0;   ///< outputs of any kind whose bytes did not need rewriting
    int removed = 0;     ///< outputs this run deleted because they are no longer published
};

using ProgressCallback = std::function<void(int done, int total)>;

/// Exports a vault - or its `publish: true` subset - to a static website.
/// Never touches the vault: every read goes through `Vault`/`Index`, every
/// write goes into `Options::outputDir`. See publish() for the incremental
/// and privacy guarantees, which are the two properties this class exists to
/// get right; everything else (HTML shape, CSS) is in PageBody and the
/// site-chrome helpers in Publisher.cpp.
class Publisher {
public:
    Publisher(const noteAppC::Vault &vault, const noteAppC::Index &index);

    /// Runs one publish. `progress` is called from this thread only (never
    /// from a worker), with `done`/`total` counted in published notes, so it
    /// is safe to update UI from directly. Returns false with `*error` set on
    /// a refused output directory or an I/O failure; a page that individually
    /// fails to render (should not happen - PageBody has no failure path
    /// beyond falling back to source text) never aborts the whole run.
    bool publish(const Options &options, const ProgressCallback &progress, QString *error);

    Counts counts() const { return m_counts; }

private:
    const noteAppC::Vault &m_vault;
    const noteAppC::Index &m_index;
    Counts m_counts;
};

} // namespace noteAppC::publish
