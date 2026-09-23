#include "KernelClient.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QMutexLocker>
#include <QUuid>

#include <zmq.h>

namespace noteAppC::notebook {

namespace {

bool sendFrames(void *socket, const QList<QByteArray> &frames)
{
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
        const QByteArray &frame = frames.at(i);
        if (zmq_send(socket, frame.constData(), size_t(frame.size()), flags) < 0)
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
        if (zmq_msg_recv(&msg, socket, 0) < 0) {
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

QString KernelClient::execute(const QString &code)
{
    QJsonObject content;
    content.insert(QStringLiteral("code"), code);
    content.insert(QStringLiteral("silent"), false);
    content.insert(QStringLiteral("store_history"), true);
    content.insert(QStringLiteral("user_expressions"), QJsonObject());
    content.insert(QStringLiteral("allow_stdin"), false);
    content.insert(QStringLiteral("stop_on_error"), true);

    const QJsonObject header = newHeader(QStringLiteral("execute_request"), m_session, m_username);
    const KernelMessage message = buildMessage(header, {}, {}, content);

    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Shell, message});
    return header.value(QStringLiteral("msg_id")).toString();
}

void KernelClient::requestInterrupt()
{
    const QJsonObject header = newHeader(QStringLiteral("interrupt_request"), m_session, m_username);
    const KernelMessage message = buildMessage(header, {}, {}, QJsonObject());
    QMutexLocker lock(&m_mutex);
    m_outbox.append({Channel::Control, message});
}

void KernelClient::dispatchShellOrControl(const KernelMessage &message)
{
    const QString type = message.msgType();
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
    } else if (type == QLatin1String("display_data")) {
        emit displayDataReceived(parent, content.value(QStringLiteral("data")).toObject());
    } else if (type == QLatin1String("error")) {
        emit errorReceived(parent, content.value(QStringLiteral("ename")).toString(),
                           content.value(QStringLiteral("evalue")).toString(), tracebackOf(content));
    }
    // "execute_input", "clear_output" and the rest: not shown - a notebook
    // block already shows its own source, and nothing here clears output
    // mid-run rather than replacing it wholesale once the run finishes.
}

void KernelClient::run()
{
    void *ctx = zmq_ctx_new();
    void *shell = zmq_socket(ctx, ZMQ_DEALER);
    void *control = zmq_socket(ctx, ZMQ_DEALER);
    void *iopub = zmq_socket(ctx, ZMQ_SUB);
    void *hb = zmq_socket(ctx, ZMQ_REQ);
    const int linger = 0;
    for (void *socket : {shell, control, iopub, hb})
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(iopub, ZMQ_SUBSCRIBE, "", 0);

    const QByteArray shellEp = m_info.shellEndpoint().toUtf8();
    const QByteArray controlEp = m_info.controlEndpoint().toUtf8();
    const QByteArray iopubEp = m_info.iopubEndpoint().toUtf8();
    QByteArray hbEp = m_info.hbEndpoint().toUtf8();
    zmq_connect(shell, shellEp.constData());
    zmq_connect(control, controlEp.constData());
    zmq_connect(iopub, iopubEp.constData());
    zmq_connect(hb, hbEp.constData());

    QElapsedTimer sinceHeartbeat;
    sinceHeartbeat.start();
    bool hbAwaiting = false;
    bool hbAlive = true;

    // ponytail: a fixed 50 ms poll tick, not a wake-on-write pipe - simplest
    // thing that keeps both outgoing sends and the heartbeat's 1 s cadence
    // well within an interactive feel; add an eventfd/pair-socket wakeup if
    // sub-50ms send latency ever matters.
    while (!m_stopRequested.load()) {
        {
            QMutexLocker lock(&m_mutex);
            for (const PendingSend &pending : m_outbox)
                sendFrames(pending.channel == Channel::Shell ? shell : control,
                          encodeMessage(pending.message, m_info.key));
            m_outbox.clear();
        }

        if (!hbAwaiting && sinceHeartbeat.elapsed() > 1000) {
            zmq_send(hb, "ping", 4, 0);
            hbAwaiting = true;
            sinceHeartbeat.restart();
        }

        zmq_pollitem_t items[4] = {
            {shell, 0, ZMQ_POLLIN, 0},
            {control, 0, ZMQ_POLLIN, 0},
            {iopub, 0, ZMQ_POLLIN, 0},
            {hb, 0, ZMQ_POLLIN, 0},
        };
        if (zmq_poll(items, 4, 50) < 0)
            break; // context torn down under us, or a genuine error - either way, stop

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
        if (items[3].revents & ZMQ_POLLIN) {
            char buf[64];
            zmq_recv(hb, buf, sizeof(buf), 0); // the pong's payload is not checked, only its arrival
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
            zmq_connect(hb, hbEp.constData());
            hbAwaiting = false;
            sinceHeartbeat.restart();
            if (hbAlive) {
                hbAlive = false;
                emit heartbeatAlive(false);
            }
        }
    }

    zmq_close(shell);
    zmq_close(control);
    zmq_close(iopub);
    zmq_close(hb);
    zmq_ctx_destroy(ctx);
}

} // namespace noteAppC::notebook
