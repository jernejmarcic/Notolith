#pragma once

// Owns one kernel per language, lazily: nothing starts until the first
// forceRun=true request for that language (stage 13's performance rule -
// "no kernel starts before the user runs something"). This is the type the
// window (libs/ui) actually talks to; MarkdownEditor::setNotebookRunner's
// callback is a thin adapter over run() below (see docs on that seam in
// MarkdownEditor.h and the wiring snippet in the lane's own report).

#include "DirectRunner.h"
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

    /// What a kernel's input() asks, and how the answer goes back.
    struct InputPrompt {
        QString prompt;
        bool password = false; ///< getpass: what is typed is not shown
    };
    /// `ok` false (Esc, the prompt dismissed) interrupts the run instead of
    /// answering it - Jupyter's own way out of an input() nobody will answer.
    /// Calling it a second time does nothing.
    using InputReply = std::function<void(bool ok, const QString &line)>;
    using InputHandler = std::function<void(const InputPrompt &, const InputReply &)>;
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
        /// SandboxTier::Wasm only: the `notebooks.wasmModules` setting as
        /// written (`python=/p/python.wasm;...`); the block's own entry is
        /// looked up in it. Blocks in this tier never use a kernel.
        QString wasmModules;
        /// Through the language's Jupyter kernel even when the system's own
        /// tool could run it: what the kernel's end-to-end test asks for, and
        /// what a note asks for with `#| kernel: on` (or the setting that
        /// shares one kernel between a note's blocks).
        bool useKernel = false;
        /// Whose kernel it is: blocks with the same scope (a note's path),
        /// tier and kernel share one process, so a variable one block set
        /// is there for the next. Empty is one kernel for everybody, as a
        /// single-notebook Jupyter has.
        QString scope;
        /// A kernelspec's name (`python3`, `ir`) instead of the one the
        /// language finds; implies a kernel.
        QString kernelName;
        /// 0: the runner's own limit (30 s without a kernel, none with one,
        /// as in Jupyter, where Interrupt is the way out).
        int timeoutMs = 0;
        /// false (`#| cache: false`): a lookup finds nothing and a run is not kept.
        bool cache = true;
        /// Set: a kernel run may ask for a line (input()) and this is called, on
        /// the controller's thread, for each ask. Unset: the kernel is told
        /// stdin is not there and input() raises. Only a kernel can ask: the
        /// system's own one-shot runner has no terminal behind it.
        InputHandler onInput;
    };
    using Deliver = std::function<void(NotebookRunResult)>;

    /// `forceRun` false: a cache-only lookup, delivered on the next event
    /// loop tick (never synchronously, never starting a kernel) - what a
    /// note reopening calls for every notebook block it lays out. `true`:
    /// runs for real, launching or reusing `request.language`'s kernel.
    void run(const RunRequest &request, bool forceRun, const Deliver &deliver);

    /// SIGINT (or, for a kernel that understands it, interrupt_request) to
    /// `language`'s kernel, if one is running, and stops a run that has no
    /// kernel. `scope` empty: every note's. No effect otherwise.
    void interruptKernel(const QString &language, const QString &scope = QString());
    /// Stops `language`'s kernel (the next run starts a fresh one, with none
    /// of the old variables); every in-flight request on it is delivered a
    /// kernel-level error rather than left hanging. `scope` empty: every note's.
    void restartKernel(const QString &language, const QString &scope = QString());
    /// How many kernels are running for `scope` (empty: all of them).
    int runningKernelCount(const QString &scope = QString()) const;
    /// Every kernel this controller owns, stopped. Called once, from
    /// ~MainWindow or equivalent, before the process that owns their
    /// sockets and runtime directories goes away.
    void shutdownAll();

    bool hasRunningKernel(const QString &language) const;

    /// What a kernel answers to complete_request / inspect_request.
    struct Introspection {
        bool ok = false;       ///< false: no answer worth showing (cancelled, timed out, kernel gone)
        QStringList matches;   ///< completion: the candidates
        qsizetype from = 0;    ///< completion: what they replace, in `code` (UTF-16 offsets)
        qsizetype to = 0;
        QString text;          ///< inspect: the documentation, plain text; empty when nothing was found
    };
    using DeliverIntrospection = std::function<void(Introspection)>;
    /// Completion (`inspect` false) or documentation (true) for `code` with the
    /// caret at `cursor`, asked of the kernel `request` names (language, scope,
    /// tier, kernel) - and only if that kernel is already running: this never
    /// starts one. False, with nothing sent, when there is none. The answer
    /// comes later on the event loop, exactly once; a newer request of the same
    /// kind to the same kernel cancels the older (it answers ok = false), and
    /// so does a kernel that is busy for more than a few seconds.
    bool introspect(const RunRequest &request, const QString &code, qsizetype cursor, bool inspect,
                    const DeliverIntrospection &deliver);

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
        bool cache = true;
        bool timedOut = false;
        bool truncated = false;
        InputHandler onInput;
        int timeoutMs = 0;
        int executionCount = 0;
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
        QString key;      ///< scope | tier | kernelspec name - m_kernels' own key
        QString scope;
        QString language; ///< the spec's, lower-cased
        std::unique_ptr<KernelProcess> process;
        std::unique_ptr<KernelClient> client;
        KernelSpec spec;
        SandboxTier tierRequested = SandboxTier::Bubblewrap;
        QHash<QString, InFlight> inFlight; ///< by msg_id
        struct Pending {
            DeliverIntrospection deliver;
            bool inspect = false;
            QString code; ///< to turn the kernel's code point offsets back into QString ones
        };
        QHash<QString, Pending> introspections; ///< by msg_id
    };

    KernelSession *ensureKernel(const RunRequest &request, QString *error);
    void wireKernel(KernelSession *session);
    void finalizeIfDone(KernelSession *session, QHash<QString, InFlight>::iterator it);
    void failInFlight(KernelSession *session, const QString &error);
    KernelSession *runningKernelFor(const RunRequest &request);
    void interruptSession(KernelSession *session);

    /// std::map, not QHash: QHash's copy-on-write detach needs a copyable
    /// value, and std::unique_ptr<KernelSession> is deliberately not one -
    /// two lookups for the price of a Big-O nobody will notice at "one
    /// kernel per language".
    std::map<QString, std::unique_ptr<KernelSession>> m_kernels;
    DirectRunner *m_direct = nullptr; ///< made on the first run that needs it
};

} // namespace noteAppC::notebook
