#pragma once

#include <QList>
#include <QString>

#include "Metadata.h"

namespace noteAppC {

/// Obsidian's own syntax, which is not CommonMark and which md4qt therefore
/// leaves as plain text: `[[wikilinks]]`, `![[embeds]]`, `#tags`,
/// `==highlights==`, `%%comments%%` and `^block-ids`.
///
/// md4qt 5 has no plugin hook, so this runs as a pass over the source with the
/// ranges md4qt already claimed (code, math, raw HTML, frontmatter) masked
/// out. Scanning the source rather than the parsed text is what keeps every
/// offset true, which the live-preview editor of Phase 2 depends on.
class ObsidianSyntax {
public:
    struct Result {
        QList<Link> links;
        QList<Tag> tags;
        QList<BlockId> blockIds;
        QList<Range> highlights;
        QList<Range> comments;
    };

    /// `masked` ranges are skipped; they need not be sorted.
    static Result scan(const QString &text, const QList<Range> &masked);

    /// Splits `[[target#subpath|alias]]` into its parts. `raw` excludes the
    /// brackets. Exposed for the editor's suggesters and the tests.
    static Link parseWikilink(const QString &raw, bool embed);

    /// True for a target Obsidian would open in a browser rather than resolve
    /// inside the vault.
    static bool isExternalTarget(const QString &target);
};

} // namespace noteAppC
