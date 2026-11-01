#pragma once

// Kernel discovery: any installed Jupyter kernel is a language, found the
// way `jupyter --paths` and jupyter_client.kernelspec.KernelSpecManager find
// one - a `kernel.json` under a `kernels/<name>/` directory in one of a
// fixed list of roots. No Python runs to do this: the roots and the
// kernel.json shape are just read directly (no Python in the app itself).

#include <QHash>
#include <QString>
#include <QStringList>

namespace noteAppC::notebook {

struct KernelSpec {
    QString name;        ///< the directory name, e.g. "python3"
    QString displayName; ///< kernel.json's "display_name"
    QString language;    ///< kernel.json's "language", e.g. "python" - matched
                         ///< against a fence's info-string language, case-insensitive
    QStringList argv;    ///< launch command; "{connection_file}" is a placeholder
    /// "signal" (default, POSIX: interrupt is SIGINT to the process) or
    /// "message" (an interrupt_request on the control channel). The
    /// installed python3 kernelspec does not set this, so it is "signal".
    QString interruptMode = QStringLiteral("signal");
    QString kernelJSONpath; ///< for diagnostics: where this was read from
};

/// Every root `jupyter --paths` would list under "data": `$JUPYTER_PATH`
/// entries first (colon-separated, each given a trailing `/kernels` the way
/// jupyter_core does), then `$JUPYTER_DATA_DIR` (or
/// `$XDG_DATA_HOME/jupyter`, or `~/.local/share/jupyter`), then the system
/// ones - `/usr/local/share/jupyter` and `/usr/share/jupyter` on Linux.
/// Exposed so a test can point `JUPYTER_PATH`/`JUPYTER_DATA_DIR` at a temp
/// tree without ever touching the real one.
QStringList kernelSpecRoots();

/// Every kernelspec found under kernelSpecRoots(), most-specific root first
/// (a `JUPYTER_PATH`/user one shadows a system one of the same name, the
/// same precedence jupyter_client uses) - one entry per distinct `name`.
QList<KernelSpec> discoverKernelSpecs();

/// discoverKernelSpecs(), indexed by language (lower-cased), first match per
/// language wins. What NotebookController actually looks a fence's language
/// up in.
QHash<QString, KernelSpec> kernelSpecsByLanguage();

/// `spec.argv` with every literal "{connection_file}" replaced by `path`.
QStringList substituteConnectionFile(const QStringList &argv, const QString &path);

} // namespace noteAppC::notebook
