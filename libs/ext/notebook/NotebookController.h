// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// Owns one kernel per language, lazily: nothing starts until the first
// forceRun=true request for that language. This is the type the
// window (libs/ui) actually talks to; MarkdownEditor::setNotebookRunner's
// callback is a thin adapter over run() below (see docs on that seam in
// MarkdownEditor.h).

#include "DirectRunner.h"
#include "KernelClient.h"
#include "KernelProcess.h"
#include "KernelSpec.h"
#include "NotebookCache.h"
#include "NotebookSandbox.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>

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
        /// The note's implicit code (frontmatter `implicit-<lang>` and
        /// `implicit-<lang>-main`): `prelude` goes above the block, `wrapper`
        /// (with `{{code}}`) is what a block with no entry point goes into.
        /// Empty: the block runs exactly as written. A kernel gets the prelude
        /// once per kernel (and again after a restart), never with each block.
        QString prelude;
        QString wrapper;
        /// Which of the note's identical blocks this is (duplicateOrdinal): part
        /// of the output cache's key and nothing else - it never reaches the kernel.
        int duplicateOrdinal = 0;
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
        /// A run of a block over this many bytes of UTF-8 source is refused (oversizeMessage): the setting
        /// `notebooks.maxRunnableBytes`, whose default this is. 0 is no limit. Checked in run(), for every caller.
        qint64 maxSourceBytes = 65536;
        /// What asked for the run (the editor), handed to the run gate; never read otherwise.
        QPointer<QObject> origin;
    };
    using Deliver = std::function<void(NotebookRunResult)>;
    /// Asked in run(), for every `forceRun` request and after the size limit, before anything starts: empty
    /// lets the run go on, a text refuses it and is shown as the block's error. What a vault that is not trusted
    /// asks the person lives here, so no caller can skip it. May show a dialog: called on the caller's thread.
    using RunGate = std::function<QString(const RunRequest &)>;
    void setRunGate(RunGate gate) { m_runGate = std::move(gate); }

    /// `forceRun` false: a cache-only lookup, delivered on the next event
    /// loop tick (never synchronously, never starting a kernel) - what a
    /// note reopening calls for every notebook block it lays out. `true`:
    /// runs for real, launching or reusing `request.language`'s kernel.
    void run(const RunRequest &request, bool forceRun, const Deliver &deliver);
    /// The output cache's key a run of `request` is kept under.
    static QString cacheKeyOf(const RunRequest &request);

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
    /// A settings/status row can use this later; nothing in this module reads
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
        bool timeoutArmed = false;
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
        QString preludeSent;                    ///< the implicit code this kernel has run
        QList<NotebookOutput> preludeError;     ///< its failure, shown on the next block's result
    };

    KernelSession *ensureKernel(const RunRequest &request, QString *error);
    void wireKernel(KernelSession *session);
    void finalizeIfDone(KernelSession *session, QHash<QString, InFlight>::iterator it);
    void failInFlight(KernelSession *session, const QString &error);
    KernelSession *runningKernelFor(const RunRequest &request);
    void interruptSession(KernelSession *session);
    void armTimeout(KernelSession *session, const QString &msgID, int timeoutMs); ///< from the kernel's "busy" for that request

    /// std::map, not QHash: QHash's copy-on-write detach needs a copyable
    /// value, and std::unique_ptr<KernelSession> is deliberately not one -
    /// two lookups for the price of a Big-O nobody will notice at "one
    /// kernel per language".
    std::map<QString, std::unique_ptr<KernelSession>> m_kernels;
    DirectRunner *m_direct = nullptr; ///< made on the first run that needs it
    RunGate m_runGate;
};

} // namespace noteAppC::notebook
