#pragma once

#include "NotebookCache.h"
#include "NotebookSandbox.h"

#include <QObject>
#include <QStringList>

#include <functional>

class QProcess;

namespace noteAppC::notebook {

/// Runs a code block with the system's own interpreter or compiler - one
/// short process per run, in the sandbox the block asked for - rather than
/// through a Jupyter kernel: nothing to install beyond the language itself,
/// nothing left running between runs. Jernej, 2026-09-24: "Not jupyter
/// kernel, should use system itself ... most efficient way".
///
/// A block behaves as a notebook cell does where the language allows it: a
/// Python block's last expression and every Haskell expression print their
/// value. Blocks share nothing: each run is a fresh process.
class DirectRunner : public QObject {
    Q_OBJECT

public:
    explicit DirectRunner(QObject *parent = nullptr);
    ~DirectRunner() override;

    /// The tool that runs `language` ("runghc", "python3"...), whether or not
    /// it is installed; empty for a language this does not know.
    static QString toolFor(const QString &language);
    /// Known and installed.
    static bool canRun(const QString &language);

    using Deliver = std::function<void(NotebookRunResult)>;
    struct Request {
        QString language;
        QString source;
        SandboxTier tier = SandboxTier::Bubblewrap;
        QStringList readOnlyBinds;
        int timeoutMs = 30'000;
    };
    /// Runs it; `deliver` gets stdout as text and, when it fails, stderr as
    /// the error. Always answered, on this object's thread.
    void run(const Request &request, const Deliver &deliver);
    /// Kills whatever is running for `language`.
    void interrupt(const QString &language);

private:
    struct Running;
    QList<Running *> m_running;
};

} // namespace noteAppC::notebook
