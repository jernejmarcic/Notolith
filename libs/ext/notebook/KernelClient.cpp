// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "KernelClient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QMutexLocker>
#include <QUuid>

#include <zmq.h>

#include <algorithm>
#include <cerrno>

#include <sys/stat.h>
#include <unistd.h>

namespace noteAppC::notebook {

EndpointState checkEndpoint(const QString &endpoint)
{
    static const QLatin1String scheme("ipc://");
    if (!endpoint.startsWith(scheme))
        return EndpointState::Plain;
    struct stat info;
    if (lstat(QFile::encodeName(endpoint.mid(scheme.size())).constData(), &info) != 0)
        return EndpointState::Missing;
    return S_ISSOCK(info.st_mode) && info.st_uid == geteuid() ? EndpointState::Plain : EndpointState::Refused;
}

namespace {

/// Retries a zmq_* call across a spurious EINTR - a signal interrupting the
/// underlying blocking syscall, which a multi-threaded Qt app sees often
/// (QProcess's own SIGCHLD handling is enough) - rather than treating an
/// interrupted call as a real failure. Found by hand, not guessed at: the
/// first version of this loop `break`d the whole run() loop on any negative
/// return, which under real load intermittently killed the worker thread
/// mid-run and made every message after that point vanish. `fn` follows
/// zmq's own convention: >= 0 is success, -1 with zmq_errno() set is not.
template <typename Fn>
int zmqRetryEINTR(Fn &&fn)
{
    while (true) {
        const int rc = fn();
        if (rc >= 0 || zmq_errno() != EINTR)
            return rc;
    }
}

bool sendFrames(void *socket, const QList<QByteArray> &frames)
{
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
        const QByteArray &frame = frames.at(i);
        const int rc = zmqRetryEINTR(
            [&] { return zmq_send(socket, frame.constData(), size_t(frame.size()), flags); });
        if (rc < 0)
            return false;
    }
    return true;
}

/// One multipart message, blocking - only called once zmq_poll already said
/// a first frame is ready, so it never actually blocks in practice.
QList<QByteArray> recvFrames(void *socket)
{
    QList<QByteArray> frames;
    while (true) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        if (zmqRetryEINTR([&] { return zmq_msg_recv(&msg, socket, 0); }) < 0) {
            zmq_msg_close(&msg);
            break;
        }
        frames.append(QByteArray(static_cast<const char *>(zmq_msg_data(&msg)), int(zmq_msg_size(&msg))));
        int more = 0;
        size_t moreSize = sizeof(more);
        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moreSize);
        zmq_msg_close(&msg);
        if (!more)
            break;
    }
    return frames;
}

/// Content's "traceback" is a list of already-ANSI-coloured lines in the
/// real protocol; noteAppC strips escapes when it paints them (see
/// NotebookBlock.cpp's stripAnsi), so it is read here as plain strings.
QStringList tracebackOf(const QJsonObject &content)
{
    QStringList lines;
    for (const QJsonValue &value : content.value(QStringLiteral("traceback")).toArray())
        lines << value.toString();
    return lines;
}

} // namespace

KernelClient::KernelClient(QObject *parent) : QThread(parent) { }

KernelClient::~KernelClient()
{
    stopClient();
}

void KernelClient::startClient(const KernelConnectionInfo &info, const QString &session,
                               const QString &username)
{
    m_info = info;
    m_session = session;
    m_username = username;
    m_stopRequested.store(false);
    start();
}

void KernelClient::stopClient()
{
    if (!isRunning())
        return;
    m_stopRequested.store(true);
    wait(2000);
}

QString KernelClient::execute(const QString &code, bool allowStdin, bool storeHistory)
{
    QJsonObject content;
    content.insert(QStringLiteral("code"), code);
    content.insert(QStringLiteral("silent"), false);
    content.insert(QStringLiteral("store_history"), storeHistory);
    content.insert(QStringLiteral("user_expressions"), QJsonObject());
    content.insert(QStringLiteral("allow_stdin"), allowStdin);
    // Blocks are independent cells: one failing must not abort the ones
    // queued behind it (a "run all" with an error in the middle).
    content.insert(QStringLiteral("stop_on_error"), false);

    const QJsonObject header = newHeader(QStringLiteral("execute_request"), m_session, m_username);
    const KernelMessage message = buildMessage(header, {}, {}, content);

    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Shell, message});
    return header.value(QStringLiteral("msg_id")).toString();
}

QString KernelClient::complete(const QString &code, int cursorPos)
{
    QJsonObject content;
    content.insert(QStringLiteral("code"), code);
    content.insert(QStringLiteral("cursor_pos"), cursorPos);
    const QJsonObject header = newHeader(QStringLiteral("complete_request"), m_session, m_username);
    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Shell, buildMessage(header, {}, {}, content)});
    return header.value(QStringLiteral("msg_id")).toString();
}

QString KernelClient::inspect(const QString &code, int cursorPos)
{
    QJsonObject content;
    content.insert(QStringLiteral("code"), code);
    content.insert(QStringLiteral("cursor_pos"), cursorPos);
    content.insert(QStringLiteral("detail_level"), 0);
    const QJsonObject header = newHeader(QStringLiteral("inspect_request"), m_session, m_username);
    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Shell, buildMessage(header, {}, {}, content)});
    return header.value(QStringLiteral("msg_id")).toString();
}

void KernelClient::requestInterrupt()
{
    const QJsonObject header = newHeader(QStringLiteral("interrupt_request"), m_session, m_username);
    const KernelMessage message = buildMessage(header, {}, {}, QJsonObject());
    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Control, message});
}

void KernelClient::replyToInput(const QString &line)
{
    QJsonObject content;
    content.insert(QStringLiteral("value"), line);
    content.insert(QStringLiteral("status"), QStringLiteral("ok"));
    const QJsonObject header = newHeader(QStringLiteral("input_reply"), m_session, m_username);
    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Stdin, buildMessage(header, {}, {}, content)});
}

void KernelClient::dispatchShellOrControl(const KernelMessage &message)
{
    const QString type = message.msgType();
    if (type == QLatin1String("complete_reply") || type == QLatin1String("inspect_reply")) {
        emit introspectionReplyReceived(message.parentMsgID(), type, message.content());
        return;
    }
    if (type != QLatin1String("execute_reply"))
        return; // interrupt_reply and friends: nothing here needs them yet
    const QJsonObject content = message.content();
    const bool ok = content.value(QStringLiteral("status")).toString() == QLatin1String("ok");
    const int count = content.value(QStringLiteral("execution_count")).toInt(-1);
    emit executeReplyReceived(message.parentMsgID(), ok, count);
}

void KernelClient::dispatchIopub(const KernelMessage &message)
{
    const QString type = message.msgType();
    const QString parent = message.parentMsgID();
    const QJsonObject content = message.content();

    if (type == QLatin1String("status")) {
        emit statusChanged(parent, content.value(QStringLiteral("execution_state")).toString());
    } else if (type == QLatin1String("stream")) {
        emit streamOutput(parent, content.value(QStringLiteral("name")).toString(),
                          content.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("execute_result")) {
        emit executeResultReceived(parent, content.value(QStringLiteral("data")).toObject(),
                                   content.value(QStringLiteral("execution_count")).toInt(-1));
    } else if (type == QLatin1String("display_data") || type == QLatin1String("update_display_data")) {
        const QString displayID = content.value(QStringLiteral("transient")).toObject()
                                      .value(QStringLiteral("display_id")).toString();
        emit displayDataReceived(parent, content.value(QStringLiteral("data")).toObject(), displayID,
                                 type == QLatin1String("update_display_data"));
    } else if (type == QLatin1String("clear_output")) {
        emit clearOutputReceived(parent);
    } else if (type == QLatin1String("error")) {
        emit errorReceived(parent, content.value(QStringLiteral("ename")).toString(),
                           content.value(QStringLiteral("evalue")).toString(), tracebackOf(content));
    }
    // "execute_input" and the rest: not shown - a block already shows its own
    // source.
}

void KernelClient::run()
{
    const QStringList endpoints = {m_info.shellEndpoint(), m_info.controlEndpoint(), m_info.stdinEndpoint(),
                                   m_info.iopubEndpoint(), m_info.hbEndpoint()};
    const auto replaced = [&endpoints] {
        return std::ranges::any_of(endpoints, [](const QString &endpoint) { return checkEndpoint(endpoint) == EndpointState::Refused; });
    };
    bool rogue = false; // an endpoint was swapped for something that is not a socket of ours
    // Connect only to what is there and is a socket: the kernel makes the files as it starts, so wait for all five,
    // and refuse (and stop) at a link or anything else in their place. A swap later is caught in the loop below.
    for (bool ready = false; !ready;) {
        ready = true;
        for (const QString &endpoint : endpoints) {
            switch (checkEndpoint(endpoint)) {
            case EndpointState::Refused:
                qWarning("kernel: %s is not a socket of ours; not connecting", qPrintable(endpoint));
                emit heartbeatAlive(false);
                return;
            case EndpointState::Missing:
                ready = false;
                break;
            case EndpointState::Plain:
                break;
            }
        }
        if (!ready) {
            if (m_stopRequested.load())
                return;
            msleep(10);
        }
    }

    void *ctx = zmq_ctx_new();
    void *shell = zmq_socket(ctx, ZMQ_DEALER);
    void *control = zmq_socket(ctx, ZMQ_DEALER);
    void *stdinSocket = zmq_socket(ctx, ZMQ_DEALER);
    void *iopub = zmq_socket(ctx, ZMQ_SUB);
    void *hb = zmq_socket(ctx, ZMQ_REQ);
    const int linger = 0;
    // A dropped connection is retried after a second, not a tenth of one: the loop below looks at the five
    // paths every tick, so a link put in place of a socket that went away is seen before it is followed.
    const int reconnect = 1000;
    for (void *socket : {shell, control, stdinSocket, iopub, hb}) {
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(socket, ZMQ_RECONNECT_IVL, &reconnect, sizeof(reconnect));
    }
    // One identity on every DEALER, as jupyter_client gives them: the kernel
    // answers an execute_request's input() on its stdin ROUTER to the identity
    // the request came from, which only reaches the stdin socket if that is the same.
    const QByteArray identity = m_session.toUtf8();
    for (void *socket : {shell, control, stdinSocket})
        zmq_setsockopt(socket, ZMQ_IDENTITY, identity.constData(), size_t(identity.size()));
    zmq_setsockopt(iopub, ZMQ_SUBSCRIBE, "", 0);

    const QByteArray shellEp = m_info.shellEndpoint().toUtf8();
    const QByteArray controlEp = m_info.controlEndpoint().toUtf8();
    const QByteArray iopubEp = m_info.iopubEndpoint().toUtf8();
    QByteArray hbEp = m_info.hbEndpoint().toUtf8();
    // The stdin socket has the same race as iopub below, on the other side: the
    // kernel's stdin ROUTER drops an input_request addressed to an identity
    // whose handshake has not finished, and input() then waits forever. On a
    // loaded machine the first run's input() lost it most of the time. So the
    // outbox waits for this socket's handshake as it waits for iopub_welcome.
    zmq_socket_monitor(stdinSocket, "inproc://stdin-handshake", ZMQ_EVENT_HANDSHAKE_SUCCEEDED);
    void *stdinMonitor = zmq_socket(ctx, ZMQ_PAIR);
    zmq_setsockopt(stdinMonitor, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_connect(stdinMonitor, "inproc://stdin-handshake");
    zmq_connect(shell, shellEp.constData());
    zmq_connect(control, controlEp.constData());
    zmq_connect(stdinSocket, m_info.stdinEndpoint().toUtf8().constData());
    zmq_connect(iopub, iopubEp.constData());
    zmq_connect(hb, hbEp.constData());

    // The classic ZMQ PUB/SUB "slow joiner" problem, confirmed by hand (a
    // Python jupyter_client script never hits it - it awaits kernel_info
    // first, which this client does not send - and looping this client
    // alone showed the very first execute_request's iopub side going
    // missing on roughly one run in three): a SUB's subscription takes a
    // moment to reach the kernel's XPUB iopub socket, and anything the
    // kernel publishes before it arrives - the busy/idle status for a
    // request sent immediately after connecting, say - is dropped with no
    // redelivery. ipykernel's own fix for this is an "iopub_welcome"
    // message, sent once a fresh subscription is actually registered
    // (ipykernel/iostream.py, IOPubThread._send_welcome_message) - waited
    // for here, once, before the outbox is ever drained, so execute()'s
    // first request cannot win a race that was never fixed. A kernel that
    // never sends one (an older ipykernel, a non-Python kernel) just falls
    // through once the timeout passes rather than hanging the connection.
    {
        QElapsedTimer readyClock;
        readyClock.start();
        bool sawWelcome = false;
        bool stdinReady = false;
        while ((!sawWelcome || !stdinReady) && readyClock.elapsed() < 3000 && !m_stopRequested.load()) {
            if ((rogue = replaced()))
                break;
            zmq_pollitem_t readyItems[] = {{iopub, 0, ZMQ_POLLIN, 0}, {stdinMonitor, 0, ZMQ_POLLIN, 0}};
            const int rc = zmqRetryEINTR([&] { return zmq_poll(readyItems, 2, 100); });
            if (rc <= 0)
                continue;
            if (readyItems[1].revents & ZMQ_POLLIN) {
                recvFrames(stdinMonitor); // event + endpoint; only the handshake is subscribed
                stdinReady = true;
            }
            if (!(readyItems[0].revents & ZMQ_POLLIN))
                continue;
            KernelMessage welcome;
            if (!decodeMessage(recvFrames(iopub), m_info.key, &welcome))
                continue;
            if (welcome.msgType() == QLatin1String("iopub_welcome"))
                sawWelcome = true;
            else
                dispatchIopub(welcome); // some other message arrived first: still real, not dropped
        }
    }

    QElapsedTimer sinceHeartbeat;
    sinceHeartbeat.start();
    bool hbAwaiting = false;
    bool hbAlive = true;

    // Limit: a fixed 50 ms poll tick, not a wake-on-write pipe - simplest
    // thing that keeps both outgoing sends and the heartbeat's 1 s cadence
    // well within an interactive feel; add an eventfd/pair-socket wakeup if
    // sub-50ms send latency ever matters.
    while (!m_stopRequested.load() && !rogue) {
        if ((rogue = replaced()))
            break;
        {
            QMutexLocker lock(&m_mutex);
            for (const PendingSend &pending : m_outbox) {
                void *socket = pending.channel == Channel::Shell ? shell
                               : pending.channel == Channel::Stdin ? stdinSocket
                                                                   : control;
                sendFrames(socket, encodeMessage(pending.message, m_info.key));
            }
            m_outbox.clear();
        }

        if (!hbAwaiting && sinceHeartbeat.elapsed() > 1000) {
            zmqRetryEINTR([&] { return zmq_send(hb, "ping", 4, 0); });
            hbAwaiting = true;
            sinceHeartbeat.restart();
        }

        zmq_pollitem_t items[5] = {
            {shell, 0, ZMQ_POLLIN, 0},
            {control, 0, ZMQ_POLLIN, 0},
            {iopub, 0, ZMQ_POLLIN, 0},
            {hb, 0, ZMQ_POLLIN, 0},
            {stdinSocket, 0, ZMQ_POLLIN, 0},
        };
        // Same zmqRetryEINTR sendFrames/recvFrames use: the first version of
        // this call `break`d the whole run() loop on any negative return,
        // which under real load intermittently killed this thread mid-run
        // (a QProcess's own SIGCHLD is enough to interrupt the syscall -
        // this thread shares a process with every QProcess the app starts)
        // and made every message after that point vanish. Caught by
        // looping the end-to-end test, not by inspection.
        const int pollResult = zmqRetryEINTR([&] { return zmq_poll(items, 5, 50); });
        if (pollResult < 0)
            break; // a genuine error (e.g. ETERM: the context was destroyed under us)

        for (void *socket : {shell, control}) {
            const bool isShell = socket == shell;
            const zmq_pollitem_t &item = isShell ? items[0] : items[1];
            if (!(item.revents & ZMQ_POLLIN))
                continue;
            // Drain what is already queued, capped so a burst on one
            // channel cannot starve iopub or the heartbeat this tick.
            for (int drained = 0; drained < 64; ++drained) {
                KernelMessage message;
                if (!decodeMessage(recvFrames(socket), m_info.key, &message))
                    break;
                dispatchShellOrControl(message);
                int more = 0;
                size_t moreSize = sizeof(more);
                zmq_getsockopt(socket, ZMQ_EVENTS, &more, &moreSize);
                if (!(more & ZMQ_POLLIN))
                    break;
            }
        }
        if (items[2].revents & ZMQ_POLLIN) {
            for (int drained = 0; drained < 256; ++drained) {
                KernelMessage message;
                if (!decodeMessage(recvFrames(iopub), m_info.key, &message))
                    break;
                dispatchIopub(message);
                int events = 0;
                size_t eventsSize = sizeof(events);
                zmq_getsockopt(iopub, ZMQ_EVENTS, &events, &eventsSize);
                if (!(events & ZMQ_POLLIN))
                    break;
            }
        }
        if (items[4].revents & ZMQ_POLLIN) {
            // The kernel waits for the answer, so a burst cannot be one the
            // loop starves anything on: at most a handful per tick.
            for (int drained = 0; drained < 8; ++drained) {
                KernelMessage message;
                if (!decodeMessage(recvFrames(stdinSocket), m_info.key, &message))
                    break;
                if (message.msgType() == QLatin1String("input_request")) {
                    const QJsonObject content = message.content();
                    emit inputRequested(message.parentMsgID(), content.value(QStringLiteral("prompt")).toString(),
                                        content.value(QStringLiteral("password")).toBool());
                }
                int events = 0;
                size_t eventsSize = sizeof(events);
                zmq_getsockopt(stdinSocket, ZMQ_EVENTS, &events, &eventsSize);
                if (!(events & ZMQ_POLLIN))
                    break;
            }
        }
        if (items[3].revents & ZMQ_POLLIN) {
            char buf[64];
            // The pong's payload is not checked, only its arrival.
            zmqRetryEINTR([&] { return zmq_recv(hb, buf, sizeof(buf), 0); });
            hbAwaiting = false;
            if (!hbAlive) {
                hbAlive = true;
                emit heartbeatAlive(true);
            }
        } else if (hbAwaiting && sinceHeartbeat.elapsed() > 5000) {
            // No pong in 5 s: the REQ/REP cycle is broken (a REQ socket that
            // sent without a matching recv cannot send again), so the
            // socket is recreated exactly as jupyter_client's own
            // HBChannel._create_socket does.
            zmq_close(hb);
            hb = zmq_socket(ctx, ZMQ_REQ);
            zmq_setsockopt(hb, ZMQ_LINGER, &linger, sizeof(linger));
            zmq_setsockopt(hb, ZMQ_RECONNECT_IVL, &reconnect, sizeof(reconnect));
            if (checkEndpoint(m_info.hbEndpoint()) == EndpointState::Refused) {
                rogue = true; // never connected to; the other four are closed below
                break;
            }
            zmq_connect(hb, hbEp.constData());
            hbAwaiting = false;
            sinceHeartbeat.restart();
            if (hbAlive) {
                hbAlive = false;
                emit heartbeatAlive(false);
            }
        }
    }

    if (rogue) {
        qWarning("kernel: an endpoint was replaced by something that is not a socket of ours; disconnected");
        emit heartbeatAlive(false);
    }
    zmq_close(shell);
    zmq_close(control);
    zmq_socket_monitor(stdinSocket, nullptr, 0);
    zmq_close(stdinMonitor);
    zmq_close(stdinSocket);
    zmq_close(iopub);
    zmq_close(hb);
    zmq_ctx_destroy(ctx);
}

} // namespace noteAppC::notebook
