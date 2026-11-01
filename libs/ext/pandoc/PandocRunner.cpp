// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "PandocRunner.h"

#include <QProcess>
#include <QTimer>

#include <utility>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace noteAppC::pandoc {

namespace {

/// Stops pandoc and what it started (pdflatex, a filter's shell): on Unix the process is its own group's leader
/// and the whole group is killed.
void killTree(QProcess *process)
{
#ifdef Q_OS_UNIX
    if (const qint64 pid = process->processId(); pid > 0 && ::kill(-pid_t(pid), SIGKILL) == 0)
        return;
#endif
    process->kill();
}

} // namespace

Runner::Runner(QObject *parent)
    : QObject(parent)
{
}

Runner::~Runner()
{
    if (m_process) {
        m_process->disconnect(this);
        killTree(m_process);
        m_process->waitForFinished(250);
    }
}

void Runner::start(const QString &program, const QStringList &arguments, const QByteArray &input,
                   const QString &workingDirectory, int timeoutMilliseconds, int filterFD, const sandbox::Limits &limits)
{
    Q_ASSERT(!m_process);
    m_cancelled = m_timedOut = false;
    m_process = new QProcess(this);
    m_process->setProgram(program);
    m_process->setArguments(arguments);
    if (!workingDirectory.isEmpty())
        m_process->setWorkingDirectory(workingDirectory);
    m_process->setStandardOutputFile(QProcess::nullDevice()); // the result goes to a file, by -o
#ifdef Q_OS_UNIX
    m_process->setChildProcessModifier([filterFD, limits] {
        ::setpgid(0, 0); // a group of its own, to kill as one
        sandbox::prepareChild(limits, filterFD);
    });
#endif
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, tr("pandoc could not be started: %1").arg(m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_cancelled)
            return finish(false, tr("Cancelled."));
        if (m_timedOut)
            return finish(false, tr("pandoc took too long and was stopped."));
        QString errors = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (status == QProcess::NormalExit && code == 0)
            return finish(true, QString());
        constexpr qsizetype kLimit = 4000;
        if (errors.size() > kLimit)
            errors = errors.left(kLimit) + QStringLiteral("...");
        finish(false, errors.isEmpty() ? tr("pandoc stopped with exit code %1.").arg(code) : errors);
    });
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        if (m_process)
            killTree(m_process);
    });
    m_timeout->start(timeoutMilliseconds);

    m_process->start();
    if (filterFD >= 0)
        ::close(filterFD); // the fork has happened; bwrap holds its own copy
    if (!m_process) // it failed to start, and finish() has said so
        return;
    if (!input.isEmpty())
        m_process->write(input);
    m_process->closeWriteChannel();
}

void Runner::cancel()
{
    if (!m_process)
        return;
    m_cancelled = true;
    killTree(m_process);
}

void Runner::finish(bool ok, const QString &message)
{
    if (!m_process)
        return;
    QProcess *process = std::exchange(m_process, nullptr);
    process->disconnect(this);
    process->deleteLater();
    delete std::exchange(m_timeout, nullptr);
    emit finished(ok, message);
}

} // namespace noteAppC::pandoc
