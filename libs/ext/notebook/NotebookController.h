#pragma once

// Owns one kernel per language, lazily: nothing starts until the first
// forceRun=true request for that language (stage 13's performance rule -
// "no kernel starts before the user runs something"). This is the type the
// window (libs/ui) actually talks to; MarkdownEditor::setNotebookRunner's
// callback is a thin adapter over run() below (see docs on that seam in
// MarkdownEditor.h and the wiring snippet in the lane's own report).

#include "KernelClient.h"
#include "KernelProcess.h"
#include "KernelSpec.h"
#include "NotebookCache.h"
#include "NotebookSandbox.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include <functional>
#include <map>
#include <memory>

namespace noteAppC::notebook {

class NotebookController : public QObject {
    Q_OBJECT
public:
    explicit NotebookController(QObject *parent = nullptr);
    ~NotebookController() override;

    struct RunRequest {
        QString language;
        QString source;
        SandboxTier tier = SandboxTier::Bubblewrap;
        /// Extra read-only binds for SandboxTier::Bubblewrap - a runnable
        /// `![[x.py]]` embed's own directory, so the file it names can
        /// actually be read (NotebookController itself never reads a file;
        /// `source` already is the file's contents by the time this is
        /// called - this is only for a script that reads *sibling* files).
        QStringList extraReadOnlyBinds;
    };
    using Deliver = std::function<void(NotebookRunResult)>;

    /// `forceRun` false: a cache-only lookup, delivered on the next event
    /// loop tick (never synchronously, never starting a kernel) - what a
    /// note reopening calls for every notebook block it lays out. `true`:
    /// runs for real, launching or reusing `request.language`'s kernel.
    void run(const RunRequest &request, bool forceRun, const Deliver &deliver);

    /// SIGINT (or, for a kernel that understands it, interrupt_request) to
    /// `language`'s kernel, if one is running. No effect otherwise.
    void interruptKernel(const QString &language);
    /// Stops and relaunches `language`'s kernel; every in-flight request on
    /// it is delivered a kernel-level error rather than left hanging.
    void restartKernel(const QString &language);
    /// Every kernel this controller owns, stopped. Called once, from
    /// ~MainWindow or equivalent, before the process that owns their
    /// sockets and runtime directories goes away.
    void shutdownAll();

    bool hasRunningKernel(const QString &language) const;

signals:
    /// A settings/status row can use this later; nothing in this lane reads
    /// it, but it costs nothing idle and is the natural place kernel health
    /// would be surfaced from.
    void kernelStateChanged(QString language, bool running);

private:
    struct InFlight {
        Deliver deliver;
        QElapsedTimer clock;
        QString cacheKey;
        QString language;
        NotebookRunResult result;
        // shell (execute_reply) and iopub (status: idle) are separate ZMQ
        // sockets with no ordering guarantee *between* them, only within
        // each - so idle can arrive before the reply, or after. Finalizing
        // on the reply alone raced against a stream/result message still on
        // its way and occasionally dropped it (caught by the end-to-end
        // test, not guessed at); waiting for both closes that race.
        bool gotReply = false;
        bool gotIdle = false;
        bool replyOk = true;
    };

    struct KernelSession {
        std::unique_ptr<KernelProcess> process;
        std::unique_ptr<KernelClient> client;
        KernelSpec spec;
        SandboxTier tierRequested = SandboxTier::Bubblewrap;
        QHash<QString, InFlight> inFlight; ///< by msg_id
    };

    KernelSession *ensureKernel(const RunRequest &request, QString *error);
    void wireKernel(KernelSession *session);
    void finalizeIfDone(KernelSession *session, QHash<QString, InFlight>::iterator it);
    void failInFlight(KernelSession *session, const QString &error);

    /// std::map, not QHash: QHash's copy-on-write detach needs a copyable
    /// value, and std::unique_ptr<KernelSession> is deliberately not one -
    /// two lookups for the price of a Big-O nobody will notice at "one
    /// kernel per language".
    std::map<QString, std::unique_ptr<KernelSession>> m_kernels;
};

} // namespace noteAppC::notebook
