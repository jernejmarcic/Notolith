#pragma once

// The output sidecar (Quarto's "freeze" idea, stage 13's requirement):
// a block's last outputs, keyed by a hash of its source and language, kept
// outside the note so opening one never runs anything and the .md stays
// clean text. Same cache root convention as libs/render/TexCompiler.cpp's
// `$XDG_CACHE_HOME/noteAppC/tex` - `$XDG_CACHE_HOME/noteAppC/notebook` here
// - chosen for the same reason and not the vault: XDG_CACHE_HOME is
// explicitly "safe to delete", which output the kernel can always
// regenerate is, and it keeps binary/PNG cache files out of a vault
// directory that Syncthing or git may also be watching (see
// feedback_build_output_in_tmp.md - the same argument against putting
// scratch state somewhere that syncs applies to a regenerable cache, not
// only to a build).

#include "NotebookSandbox.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace noteAppC::notebook {

struct NotebookOutput {
    /// New kinds go on the end: MainWindow hands them to the editor's own
    /// enum by number, and the cache names them (kindName).
    enum class Kind { Text, Markdown, ImagePng, ImageSvg, Error, Html };
    Kind kind = Kind::Text;
    QString text;         ///< text/plain or text/markdown source, an SVG's XML, or HTML
    QByteArray imageBytes; ///< Kind::ImagePng only (a PNG or a JPEG: the editor tells them apart)
    QString ename;         ///< Kind::Error only
    QString evalue;
    QStringList traceback;
    bool isStderr = false; ///< Kind::Text only: the stream was stderr (or a warning), drawn as one
    QString alt;           ///< Kind::Html only: its text/plain twin, for a build without litehtml
    QString displayID;     ///< display_data's transient display_id; update_display_data replaces by it
};

struct NotebookRunResult {
    bool hasRun = false; ///< false: never run and nothing cached - a block shows nothing extra
    bool ok = true;       ///< the execute_reply's status; false if the cell raised
    qint64 wallMs = -1;
    /// The kernel's own execution_count (`In [3]`); 0 for a run with no
    /// kernel behind it, which has no such number.
    int executionCount = 0;
    QList<NotebookOutput> outputs;
    /// A kernel-level problem (no kernel for this language, the kernel would
    /// not start) - distinct from `!ok`, which means the *code* raised.
    QString kernelError;
};

/// `language` (lower-cased) + the block's own source + `tier`, hashed - not
/// any of the display-only `#|` options (title, caption, fold, lines,
/// highlight, wrap), which must not invalidate a cached run just because a
/// reader changed how the listing looks. `tier` is folded in (stage 13's
/// rest, `#| sandbox: host`) so a host-tier and a bubblewrap-tier run of the
/// same source never share a cached result - defaults to the tier-1
/// default for the handful of existing callers that ran before tiers were
/// a per-block choice.
QString notebookCacheKey(const QString &language, const QString &source,
                         SandboxTier tier = SandboxTier::Bubblewrap);

/// Reads `key`'s cached result; `hasRun` is false and everything else
/// default when there is nothing cached - never an error, since "never run"
/// is the normal state for a block nobody has run yet.
NotebookRunResult readNotebookCache(const QString &key);

/// Writes `result` under `key`, replacing whatever was there. `hasRun` is
/// forced true regardless of the caller's own value - only a result that
/// actually ran is ever written.
void writeNotebookCache(const QString &key, NotebookRunResult result);

/// Forgets `key`'s cached result ("Clear the output"). A key with none is fine.
void removeNotebookCache(const QString &key);

/// Writes `result()` under `key` the first time `key` is read, not now: a
/// notebook's stored outputs are decoded and written only for the blocks that
/// are looked up (Ipynb.cpp). A write or removal of `key` first drops it.
void seedNotebookCache(const QString &key, std::function<NotebookRunResult()> result);

/// `$XDG_CACHE_HOME/noteAppC/notebook`, or `~/.cache/noteAppC/notebook` when
/// unset - exposed for the tests, which redirect XDG_CACHE_HOME rather than
/// touching Jernej's own cache.
QString notebookCacheRoot();

} // namespace noteAppC::notebook
