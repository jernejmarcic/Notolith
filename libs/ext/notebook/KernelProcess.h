// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Launches one kernel process: writes its connection file (always "ipc"
// transport - see the comment on connectionInfo), wraps the launch command
// through the chosen sandbox tier, and owns the QProcess. Interrupt is
// SIGINT to this process (the python3 kernelspec sets no "interrupt_mode",
// so jupyter_client's default - "signal" - applies); restart is stop() then
// start() again with a fresh connection (a stale kernel could be holding
// its old sockets a moment longer than this process takes to notice).

#include "KernelSpec.h"
#include "NotebookSandbox.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace noteAppC::notebook {

/// A connection file's contents (jupyter-client's "Connection files"): who
/// to sign messages as, and where the five channels live. Transport is
/// always "ipc" here, never "tcp" - see KernelProcess::start's comment for
/// why - so `ip` is a filesystem path prefix, not an address, and each
/// `*Port` is a small integer suffix rather than a real port number
/// (jupyter_client's own convention for ipc connection files, see
/// docs/NOTEBOOK.md... : ConnectionFileMixin._make_url).
struct KernelConnectionInfo {
    QString ip;
    QByteArray key;
    QString signatureScheme = QStringLiteral("hmac-sha256");
    QString transport = QStringLiteral("ipc");
    int shellPort = 1;
    int iopubPort = 2;
    int stdinPort = 3;
    int controlPort = 4;
    int hbPort = 5;

    QString endpoint(int port) const { return transport + QStringLiteral("://") + ip + QLatin1Char('-') + QString::number(port); }
    QString shellEndpoint() const { return endpoint(shellPort); }
    QString iopubEndpoint() const { return endpoint(iopubPort); }
    QString stdinEndpoint() const { return endpoint(stdinPort); }
    QString controlEndpoint() const { return endpoint(controlPort); }
    QString hbEndpoint() const { return endpoint(hbPort); }

    QByteArray toConnectionFileJSON(const QString &kernelName) const;
};

class KernelProcess : public QObject {
    Q_OBJECT
public:
    explicit KernelProcess(QObject *parent = nullptr);
    ~KernelProcess() override;

    /// Picks a runtime directory (under `$JUPYTER_RUNTIME_DIR`, or the
    /// system temp dir when that is unset - never a fixed, shared path, so
    /// two kernels never collide), writes the connection file into it, and
    /// launches `spec.argv` (with "{connection_file}" filled in) through
    /// `tier`. False on a launch failure (spec.argv empty, or the process
    /// would not start); `error()` then explains. `extraReadOnlyBinds` is
    /// forwarded to the sandbox as-is - a runnable `![[x.py]]` embed's own
    /// directory, say.
    bool start(const KernelSpec &spec, SandboxTier tier, const QStringList &extraReadOnlyBinds = {});
    /// SIGKILL, then waits briefly. Safe to call whether or not the process
    /// is running.
    void stop();
    bool isRunning() const;
    qint64 processId() const;
    /// SIGINT, for "signal" interrupt mode - the only mode the installed
    /// python3 kernelspec asks for. A "message" mode kernel is interrupted
    /// through KernelClient's control channel instead; this is still called
    /// (harmlessly, since nothing listens for SIGINT there) so a caller does
    /// not have to know which mode a given kernel uses before asking. The
    /// signal goes to the kernel inside bwrap's pid namespace, found through
    /// /proc, not to bwrap.
    void interrupt();

    const KernelConnectionInfo &connectionInfo() const { return m_connection; }
    SandboxTier tierUsed() const { return m_tierUsed; }
    QString error() const { return m_error; }

signals:
    /// The process exited, on its own or from stop(); never emitted by
    /// stop() itself synchronously (it always comes from QProcess::finished,
    /// queued).
    void finished(int exitCode);

private:
    /// The pid a signal actually needs to go to - see the .cpp for why this
    /// is not always processId().
    qint64 realKernelPid() const;

    std::unique_ptr<QProcess> m_process;
    std::unique_ptr<QTemporaryDir> m_runtimeDir;
    KernelConnectionInfo m_connection;
    SandboxTier m_tierUsed = SandboxTier::Bubblewrap;
    QString m_error;
};

} // namespace noteAppC::notebook
