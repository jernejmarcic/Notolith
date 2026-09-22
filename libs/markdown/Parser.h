#pragma once

#include <QString>

#include "Metadata.h"
#include "SourceMap.h"

namespace noteAppC {

/// One Markdown parser for the whole application - editor, reading view and
/// indexer - so the three can never disagree about what a note says.
///
/// md4qt handles CommonMark 0.31.2 and the GitHub extensions; everything
/// Obsidian adds on top is a pass over the source afterwards
/// (see ObsidianSyntax). Instances are cheap but not thread safe: give each
/// indexer thread its own.
class Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    /// `path` is only used for messages and for the returned metadata.
    Metadata parse(const QString &text, const QString &path = {}) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace noteAppC
