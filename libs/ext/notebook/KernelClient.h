// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ZeroMQ side of the Jupyter messaging protocol: five sockets (shell,
// control, stdin - only used for an execute() that allows it: the kernel's
// input() asks on it and the reply goes back on it - iopub, heartbeat), all
// owned by this object's own worker thread so kernel I/O is never on the
// caller's thread (a long-running cell must not block anything, and
// interrupt must keep working while one runs).
//
// Every public method is safe to call from any thread; results come back
// as ordinary Qt signals, which Qt itself marshals onto the receiver's
// thread when sender and receiver live in different ones - the standard
// mechanism, not a hand-rolled one.

#include "KernelProcess.h"
#include "NotebookMessage.h"

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QThread>

#include <atomic>

namespace noteAppC::notebook {

/// What lies at one of a kernel's endpoints. The kernel runs in a sandbox that writes the directory its ipc
/// sockets are in, so it can leave anything at the path: a link to a socket of the user's (the session bus,
/// an agent), which the app would then connect to as the user.
enum class EndpointState {
    Missing, ///< nothing there yet
    Plain,   ///< a socket this user owns and no link (or not an ipc endpoint: nothing to check)
    Refused, ///< a link, a file of another kind or another owner's: never connected to
};
/// An `ipc://path` endpoint by lstat(2), which does not follow a link.
EndpointState checkEndpoint(const QString &endpoint);

class KernelClient : public QThread {
    Q_OBJECT
public:
    explicit KernelClient(QObject *parent = nullptr);
    ~KernelClient() override;

    /// Starts the worker thread, which connects all five sockets to `info`
    /// and begins polling; returns immediately. `session` is this client's
    /// session id (stable for its lifetime, part of every message header).
    void startClient(const KernelConnectionInfo &info, const QString &session, const QString &username);
    /// Asks the worker loop to stop and waits (QThread::wait) for it to
    /// exit. Blocks the calling thread briefly - always called from the
    /// controller's own thread when a kernel is being torn down or
    /// restarted, never from inside a slot the worker itself queued.
    void stopClient();

    /// Queues an execute_request; returns its msg_id before it is actually
    /// sent (sending happens on the worker thread next tick, at most ~50 ms
    /// later) so a caller can match iopub/shell replies to this call from
    /// the moment it returns.
    /// `allowStdin`: the kernel may ask for a line (input(), getpass) while
    /// this runs - inputRequested comes back and must be answered with
    /// replyToInput or the cell waits (an interrupt ends it). Without it the
    /// kernel raises instead of asking, as before.
    /// `storeHistory` false: the run does not bump the execution count (the
    /// note's implicit code, which is not a cell of its own).
    QString execute(const QString &code, bool allowStdin = false, bool storeHistory = true);
    /// Queues a complete_request / inspect_request for `code` with the caret
    /// at `cursorPos` (Unicode code points, as the protocol counts); returns
    /// its msg_id. The answer is introspectionReplyReceived.
    QString complete(const QString &code, int cursorPos);
    QString inspect(const QString &code, int cursorPos);
    /// A control-channel interrupt_request, for a "message" interrupt-mode
    /// kernel. The installed python3 kernelspec uses "signal" mode instead
    /// (KernelProcess::interrupt(), SIGINT) - NotebookController sends both,
    /// since a kernel that does not understand interrupt_request simply
    /// never replies to it.
    void requestInterrupt();
    /// The input_reply that answers the kernel's one pending input_request.
    void replyToInput(const QString &line);

signals:
    void heartbeatAlive(bool alive);
    void statusChanged(QString parentMsgID, QString state);         ///< iopub "status": busy/idle
    void streamOutput(QString parentMsgID, QString streamName, QString text); ///< iopub "stream"
    void executeResultReceived(QString parentMsgID, QJsonObject data, int executionCount);
    /// iopub "display_data" or, `isUpdate`, "update_display_data" (a progress
    /// bar redrawn in place): `displayID` names the output it replaces.
    void displayDataReceived(QString parentMsgID, QJsonObject data, QString displayID, bool isUpdate);
    void clearOutputReceived(QString parentMsgID); ///< iopub "clear_output": tqdm, IPython.display.clear_output
    void errorReceived(QString parentMsgID, QString ename, QString evalue, QStringList traceback);
    void executeReplyReceived(QString parentMsgID, bool ok, int executionCount);
    /// stdin "input_request": the cell `parentMsgID` is waiting for a line.
    void inputRequested(QString parentMsgID, QString prompt, bool password);
    /// shell "complete_reply" / "inspect_reply": `msgType` says which.
    void introspectionReplyReceived(QString parentMsgID, QString msgType, QJsonObject content);

protected:
    void run() override;

private:
    enum class Channel { Shell, Control, Stdin };
    struct PendingSend {
        Channel channel;
        KernelMessage message;
    };

    void dispatchIopub(const KernelMessage &message);
    void dispatchShellOrControl(const KernelMessage &message);

    QMutex m_mutex;
    QList<PendingSend> m_outbox;
    std::atomic<bool> m_stopRequested{false};
    KernelConnectionInfo m_info;
    QString m_session;
    QString m_username;
};

} // namespace noteAppC::notebook
