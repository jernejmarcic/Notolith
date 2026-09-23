#include "KernelProcess.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <csignal>
#include <sys/resource.h>

namespace noteAppC::notebook {

QByteArray KernelConnectionInfo::toConnectionFileJSON(const QString &kernelName) const
{
    QJsonObject object;
    object.insert(QStringLiteral("shell_port"), shellPort);
    object.insert(QStringLiteral("iopub_port"), iopubPort);
    object.insert(QStringLiteral("stdin_port"), stdinPort);
    object.insert(QStringLiteral("control_port"), controlPort);
    object.insert(QStringLiteral("hb_port"), hbPort);
    object.insert(QStringLiteral("ip"), ip);
    object.insert(QStringLiteral("key"), QString::fromUtf8(key));
    object.insert(QStringLiteral("transport"), transport);
    object.insert(QStringLiteral("signature_scheme"), signatureScheme);
    object.insert(QStringLiteral("kernel_name"), kernelName);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

KernelProcess::KernelProcess(QObject *parent) : QObject(parent) { }

KernelProcess::~KernelProcess()
{
    stop();
}

bool KernelProcess::start(const KernelSpec &spec, SandboxTier tier, const QStringList &extraReadOnlyBinds)
{
    if (spec.argv.isEmpty()) {
        m_error = QStringLiteral("kernelspec has no argv");
        return false;
    }

    // Never a fixed shared path (two notes running blocks at once must get
    // two kernels, two runtime dirs) and never under the vault or
    // ~/Documents (nothing here is a durable artifact - see
    // feedback_build_output_in_tmp.md's reasoning, which applies just as
    // much to a kernel's live sockets as to a build). `JUPYTER_RUNTIME_DIR`
    // is honoured when set, the same variable jupyter_client itself reads,
    // which is also how the tests point this at a temp dir without loading
    // any of Jernej's own jupyter runtime state.
    QString root = qEnvironmentVariable("JUPYTER_RUNTIME_DIR");
    if (root.isEmpty())
        root = QDir::tempPath();
    QDir().mkpath(root);
    m_runtimeDir = std::make_unique<QTemporaryDir>(root + QStringLiteral("/noteAppC-kernel-XXXXXX"));
    if (!m_runtimeDir->isValid()) {
        m_error = QStringLiteral("could not create a kernel runtime directory under %1").arg(root);
        return false;
    }
    const QString dir = m_runtimeDir->path();

    // Always "ipc": unix-socket files under `dir` rather than TCP ports on
    // 127.0.0.1. Two reasons, not one - a tier-1 sandbox can then
    // --unshare-net outright (NotebookSandbox::wrapCommand's comment), and a
    // fresh network namespace's loopback interface starts down and bubblewrap
    // has no flag to bring it up, which would otherwise break tcp:// before
    // sandboxing was even the question.
    m_connection = KernelConnectionInfo{};
    m_connection.ip = dir + QStringLiteral("/kernel");
    m_connection.key = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();

    const QString connectionPath = dir + QStringLiteral("/connection.json");
    QFile connectionFile(connectionPath);
    if (!connectionFile.open(QIODevice::WriteOnly)) {
        m_error = QStringLiteral("could not write the connection file");
        return false;
    }
    connectionFile.write(m_connection.toConnectionFileJSON(spec.name));
    connectionFile.close();

    const QStringList argv = substituteConnectionFile(spec.argv, connectionPath);
    QString program;
    QStringList args;
    SandboxRequest request;
    request.runtimeDir = dir;
    request.readOnlyBinds = extraReadOnlyBinds;
    wrapCommand(tier, argv.constFirst(), argv.mid(1), request, &program, &args, &m_tierUsed);

    m_process = std::make_unique<QProcess>();
    m_process->setProgram(program);
    m_process->setArguments(args);
    m_process->setStandardInputFile(QProcess::nullDevice());
    // A kernel's own stdout/stderr are not protocol - ipykernel sends stray
    // prints there, not on the ZMQ channels - so they are just discarded
    // rather than mistaken for anything; a future settings row could surface
    // them as a log the way PluginHost does for a plugin's stderr.
    m_process->setStandardOutputFile(QProcess::nullDevice());
    m_process->setStandardErrorFile(QProcess::nullDevice());
    // Same shape as TexCompiler::compile and PluginHost::ensureStarted: a
    // ceiling applied between fork and exec, so it holds under bwrap too.
    // Generous next to PluginHost's 256 MiB - a kernel is a whole
    // interpreter, not a small out-of-process command - but still a ceiling:
    // a runaway `while True: x = x + [0]*10**6` is killed, not left to
    // pressure the rest of the machine.
    m_process->setChildProcessModifier([] {
        rlimit memory{4ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024};
        setrlimit(RLIMIT_AS, &memory);
    });
    connect(m_process.get(), &QProcess::finished, this,
           [this](int exitCode, QProcess::ExitStatus) { emit finished(exitCode); });

    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        m_error = QStringLiteral("kernel process did not start: %1").arg(m_process->errorString());
        m_process.reset();
        return false;
    }
    m_error.clear();
    return true;
}

void KernelProcess::stop()
{
    if (!m_process)
        return;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    m_process.reset();
}

bool KernelProcess::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

qint64 KernelProcess::processId() const
{
    return m_process ? m_process->processId() : 0;
}

void KernelProcess::interrupt()
{
    if (!isRunning())
        return;
    // Sent to the QProcess's own pid - under bubblewrap with --unshare-pid
    // that is bwrap itself, acting as pid 1 of the sandbox's own pid
    // namespace, which forwards SIGINT/SIGTERM/SIGHUP to the process it is
    // supervising (bwrap(1), "SIGNAL HANDLING"); under SandboxTier::Host it
    // is the kernel process directly. Either way, one code path.
    ::kill(pid_t(processId()), SIGINT);
}

} // namespace noteAppC::notebook
