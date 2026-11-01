#pragma once

#include "Bubblewrap.h"

#include <QObject>
#include <QStringList>

class QProcess;
class QTimer;

namespace noteAppC::pandoc {

/// One pandoc run at a time, started without blocking the window: the
/// program is a process of its own, written to and read through the event
/// loop, with a timeout and a way to stop it. The arguments are a list
/// handed to the program as is - no shell, so nothing in a note's text or a
/// file name is ever read as a command.
class Runner : public QObject {
    Q_OBJECT
public:
    explicit Runner(QObject *parent = nullptr);
    ~Runner() override;

    /// `input` goes to the program's standard input, which is then closed.
    /// `finished` follows once, however the run ends. A bubblewrap command
    /// (sandbox::bubblewrapCommand's) passes its `filterFD` and `limits`: they are
    /// handed to the child, and the descriptor is closed once the process has started.
    void start(const QString &program, const QStringList &arguments, const QByteArray &input,
               const QString &workingDirectory, int timeoutMilliseconds = 120000, int filterFD = -1,
               const sandbox::Limits &limits = {});
    void cancel();
    bool running() const { return m_process != nullptr; }
    /// The last run ended because cancel() was called.
    bool wasCancelled() const { return m_cancelled; }

signals:
    /// `message` is empty on success; else why not: pandoc's own words on
    /// standard error (trimmed to what fits a dialog), or that it did not
    /// start, ran out of time or was cancelled.
    void finished(bool ok, const QString &message);

private:
    void finish(bool ok, const QString &message);

    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_cancelled = false;
    bool m_timedOut = false;
};

} // namespace noteAppC::pandoc
