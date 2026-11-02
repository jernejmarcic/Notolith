// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>
#include <optional>

namespace noteAppC::pandoc {

/// What turning a note into something pandoc reads needs from outside the
/// text. All of it is asked for on the calling thread, and a function left
/// empty answers "nothing": a test, or an export of a note with no vault,
/// gives only what it has.
struct PrepareContext {
    /// The file a link or embed from `fromNote` names - note, image or any
    /// attachment, as the vault's index resolves it - or empty when none.
    std::function<QString(const QString &target, const QString &fromNote)> resolve;
    /// A note's text (LF only), for the notes an embed brings in.
    std::function<std::optional<QString>(const QString &path)> readNote;
    /// An image file for a diagram fence the editor has already drawn
    /// (never started again here), else empty. Asked for every fenced block
    /// that names a language; the answer is the caller's to cache.
    std::function<QString(const QString &language, const QString &source)> diagramImage;
    int embedDepth = 3; ///< notes embedded in notes embedded in notes
    /// What an embed may bring in: only files under this folder, symlinks resolved. A note from elsewhere must
    /// not make `![[../../.ssh/id_ed25519.pub]]` travel with an export. Empty: no check (the pure tests).
    QString vaultRoot;
};

struct PrepareResult {
    QString markdown;
    int unresolved = 0; ///< links and embeds that named nothing
    int cycles = 0;     ///< embeds left as links: a note inside itself, or too deep
};

/// `text`, the note at `notePath`, as the CommonMark pandoc reads (see
/// pandoc::inputFormat()). Only what pandoc cannot read is changed; code,
/// maths and the frontmatter are never touched:
///
///   [[Note|alias]], [[Note#H]]  a link to the note's file, relative to this note's folder,
///                               or only its text when it names nothing; [[#H]] a link to
///                               the heading's id; a block link, its text
///   ![[pic.png|300]]            an image with that width, its file's absolute path
///   ![[Other note]]             the note's text, without its frontmatter, in place (`#H`:
///                               that heading's section; `#^id`: that block);
///                               in the middle of a line, a link to it
///   ![[file.pdf]]               a link to the file
///   > [!note] Title             a quote that starts with the title in bold; `:::note`
///                               containers become pandoc divs (class callout callout-note)
///   ==text==                    a span with class mark (pandoc: highlighted)
///   %%comment%%, ^block-ids     dropped
///   ```mermaid                  the picture, when the editor already drew it
PrepareResult prepareMarkdown(const QString &text, const QString &notePath, const PrepareContext &context);

/// The id pandoc gives a heading (GitHub's rule): lower case, spaces as
/// dashes, punctuation but `-` and `_` gone. What `[[#Heading]]` links to.
QString headingID(const QString &heading);

} // namespace noteAppC::pandoc
