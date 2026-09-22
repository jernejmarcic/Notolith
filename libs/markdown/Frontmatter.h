#pragma once

#include "Metadata.h"

namespace noteAppC {

/// YAML frontmatter, Obsidian's "properties".
///
/// Obsidian only recognises a block that starts on the very first line with
/// `---` and ends at the next line that is exactly `---`.
class Frontmatter {
public:
    /// Range of the block including both fences, invalid when there is none.
    static Range find(const QString &text);

    /// Parses the block into properties, filling `aliases`, `cssClasses`,
    /// `id` and the frontmatter tags on `out`. Malformed YAML is reported in
    /// `out.parseWarnings` and leaves the rest of the metadata intact - a note
    /// with broken frontmatter still indexes.
    static void parse(const QString &text, const Range &block, Metadata &out);

    /// Type Obsidian would infer for a value. Exposed for the properties
    /// editor and for `types.json` compatibility.
    ///
    /// Parsing uses the YAML node instead, which knows what the file cannot be
    /// asked afterwards: whether a scalar was quoted, and whether a one-item
    /// list was a list.
    static Property::Type inferType(const QStringList &values);
};

} // namespace noteAppC
