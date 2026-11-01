#include "KernelProcess.h"
#include "ConfigFile.h"

#include "SyscallFilter.h"

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
#include <unistd.h>

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
    // Killing the process delivers its `finished` before this returns, into
    // slots whose owner is usually in the middle of destroying this object.
    blockSignals(true);
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
    // the home directory (nothing here is a durable artifact: a kernel's
    // live sockets belong in a runtime dir). `JUPYTER_RUNTIME_DIR`
    // is honoured when it is an absolute path, the same variable jupyter_client itself reads,
    // which is also how the tests point this at a temp dir without loading
    // any of the user's own jupyter runtime state. Else $XDG_RUNTIME_DIR/notolith, and only a
    // session with none falls back on the temporary directory.
    QString root = qEnvironmentVariable("JUPYTER_RUNTIME_DIR");
    if (!QDir::isAbsolutePath(root))
        root = appDir(XDGroot::Runtime);
    if (root.isEmpty())
        root = QDir::tempPath();
    ensurePrivateDir(root);
    m_runtimeDir = std::make_unique<QTemporaryDir>(root + QStringLiteral("/notolith-kernel-XXXXXX"));
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
    int filterFD = -1;
    wrapCommand(tier, argv.constFirst(), argv.mid(1), request, &program, &args, &m_tierUsed,
                &filterFD);
    if (program.isEmpty()) {
        m_error = QStringLiteral("code runs only inside bubblewrap's sandbox; install bubblewrap");
        return false;
    }

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
    m_process->setChildProcessModifier([filterFD] {
        rlimit memory{4ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024};
        setrlimit(RLIMIT_AS, &memory);
        sandbox::keepInChild(filterFD);
    });
    connect(m_process.get(), &QProcess::finished, this,
           [this](int exitCode, QProcess::ExitStatus) { emit finished(exitCode); });

    m_process->start();
    // The fork has happened; bwrap holds its own copy of the filter.
    if (filterFD >= 0)
        close(filterFD);
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

qint64 KernelProcess::realKernelPid() const
{
    // Under SandboxTier::Host, processId() already is the kernel. Under
    // SandboxTier::Bubblewrap it is not, even with --unshare-pid left out
    // (NotebookSandbox's own comment): bwrap still forks once - confirmed
    // by hand, not just read off its man page, which documents none of
    // this - before it execs the kernel into that child, and the outer
    // process QProcess sees has no signal handler that forwards anything
    // to it. /proc/<pid>/task/<pid>/children is the kernel-provided,
    // race-free way to name that one child without shelling out to `ps`.
    if (m_tierUsed != SandboxTier::Bubblewrap)
        return processId();
    QFile children(QStringLiteral("/proc/%1/task/%1/children").arg(processId()));
    if (!children.open(QIODevice::ReadOnly))
        return processId();
    const qint64 child = children.readAll().trimmed().split(' ').value(0).toLongLong();
    return child > 0 ? child : processId();
}

void KernelProcess::interrupt()
{
    if (!isRunning())
        return;
    ::kill(pid_t(realKernelPid()), SIGINT);
}

} // namespace noteAppC::notebook
