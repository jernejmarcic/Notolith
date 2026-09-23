#pragma once

#include "Metadata.h"

#include <optional>

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

    /// The YAML to write for `value` as a property of `type`, such that
    /// reading it back gives the same value and the same type: a text that
    /// would read as a number, a date or a checkbox is quoted, and a number,
    /// date or date-time that is not one is refused. Only those four types
    /// can be written this way.
    static std::optional<QString> scalarFor(const QString &value, Property::Type type);
};

} // namespace noteAppC
