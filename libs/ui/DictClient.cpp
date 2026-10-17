#include "DictClient.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTcpSocket>

#include <utility>

namespace noteAppC {

namespace {

constexpr qsizetype maxReply = 1 << 20; // a dictionary entry is kilobytes; a server streaming more is not one

QByteArray quoted(const QString &word)
{
    QString safe;
    for (const QChar c : word) {
        if (c.unicode() < 0x20)
            safe += QLatin1Char(' '); // a newline would start a second command
        else if (c == QLatin1Char('\\') || c == QLatin1Char('"'))
            safe += QLatin1Char('\\') + QString(c);
        else
            safe += c;
    }
    return '"' + safe.toUtf8() + '"';
}

} // namespace

QList<DictClient::Server> DictClient::parseConfig(const QString &text)
{
    QString clean;
    for (const QString &line : text.split(QLatin1Char('\n')))
        clean += line.left(line.indexOf(QLatin1Char('#')) < 0 ? line.size() : line.indexOf(QLatin1Char('#')))
                 + QLatin1Char(' ');
    clean.replace(QLatin1Char('{'), QStringLiteral(" { ")).replace(QLatin1Char('}'), QStringLiteral(" } "));
    const QStringList words = clean.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

    QList<Server> servers;
    for (qsizetype i = 0; i < words.size(); ++i) {
        if (words[i] != QLatin1String("server") || i + 1 >= words.size())
            continue;
        Server server;
        server.host = words[++i];
        if (i + 1 < words.size() && words[i + 1] == QLatin1String("{")) {
            for (i += 2; i < words.size() && words[i] != QLatin1String("}"); ++i) {
                bool ok = false;
                const int port = i + 1 < words.size() ? words[i + 1].toInt(&ok) : 0;
                if (words[i] == QLatin1String("port") && ok && port > 0 && port < 65536)
                    server.port = quint16(port);
            }
        }
        servers << server;
    }
    return servers;
}

DictClient::Server DictClient::parseServerSpec(const QString &spec)
{
    Server server;
    const QString trimmed = spec.trimmed();
    const qsizetype colon = trimmed.lastIndexOf(QLatin1Char(':'));
    bool ok = false;
    const int port = colon > 0 ? trimmed.mid(colon + 1).toInt(&ok) : 0;
    if (ok && port > 0 && port < 65536) {
        server.host = trimmed.left(colon);
        server.port = quint16(port);
    } else {
        server.host = trimmed;
    }
    return server;
}

QList<DictClient::Server> DictClient::serversFromSystem()
{
    QList<Server> servers;
    for (const QString &path : {QDir::home().filePath(QStringLiteral(".dictrc")),
                                QStringLiteral("/etc/dict/dict.conf")}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        for (const Server &server : parseConfig(QString::fromUtf8(file.readAll()))) {
            if (!servers.contains(server))
                servers << server;
        }
    }
    if (servers.isEmpty())
        servers << Server{QStringLiteral("dict.org"), 2628};
    return servers;
}

DictClient::Result DictClient::parseResponse(const QByteArray &transcript, const QString &word)
{
    Result result;
    result.word = word;
    const QStringList lines = QString::fromUtf8(transcript).split(QLatin1Char('\n'));
    // `151 "word" database "description"`
    static const QRegularExpression head(QStringLiteral("^151 \"(?:[^\"\\\\]|\\\\.)*\" (\\S+) \"(.*)\"\\s*$"));

    bool inText = false;
    Definition current;
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (inText) {
            if (line == QLatin1String(".")) {
                inText = false;
                result.definitions << current;
            } else {
                current.text += (line.startsWith(QLatin1String("..")) ? line.mid(1) : line) + QLatin1Char('\n');
            }
            continue;
        }
        if (const auto match = head.match(line); match.hasMatch()) {
            current = Definition{match.captured(1), match.captured(2), {}};
            inText = true;
        } else if (line.startsWith(QLatin1String("552"))) {
            result.error = QObject::tr("No definition found for \"%1\".").arg(word);
        } else if (result.error.isEmpty() && result.definitions.isEmpty()
                   && (line.startsWith(QLatin1String("55")) || line.startsWith(QLatin1String("50"))
                       || line.startsWith(QLatin1String("42")) || line.startsWith(QLatin1String("53")))) {
            result.error = line.mid(4).trimmed();
        }
    }
    if (result.definitions.isEmpty() && result.error.isEmpty())
        result.error = QObject::tr("No definition found for \"%1\".").arg(word);
    return result;
}

QString DictClient::toMarkdown(const Result &result)
{
    // A word is shown, never run as markup.
    const auto plain = [](const QString &text) {
        QString out;
        for (const QChar c : text) {
            if (QStringView(u"\\*_`<>$|~=").contains(c))
                out += QLatin1Char('\\');
            out += c;
        }
        return out;
    };
    QString out = QStringLiteral("## %1\n\n").arg(plain(result.word));
    if (!result.error.isEmpty())
        return out + QStringLiteral("*%1*\n").arg(plain(result.error));

    for (const Definition &definition : result.definitions) {
        out += QStringLiteral("**%1**\n\n").arg(plain(definition.description));
        // The server indents its text; indented four spaces is a code block
        // here, so the common indent goes and the rest is trimmed.
        QStringList lines = definition.text.split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
            lines.removeLast();
        for (const QString &line : std::as_const(lines)) {
            const QString text = line.trimmed();
            out += text.isEmpty() ? QStringLiteral("\n") : plain(text) + QStringLiteral("  \n");
        }
        out += QLatin1Char('\n');
    }
    return out;
}

void DictClient::define(const QString &word, std::function<void(Result)> done)
{
    dropSocket();
    m_done = std::move(done);
    m_word = word.trimmed();
    m_failures.clear();
    m_timer.setSingleShot(true);
    if (m_servers.isEmpty())
        m_servers = serversFromSystem();
    tryServer(0);
}

void DictClient::tryServer(int index)
{
    dropSocket();
    m_index = index;
    m_reply.clear();
    m_sent = false;
    if (index >= m_servers.size()) {
        QStringList hosts;
        for (const Server &server : std::as_const(m_servers))
            hosts << server.host;
        Result result;
        result.word = m_word;
        result.error = tr("Could not reach a dictionary server (%1): %2")
                           .arg(hosts.join(QStringLiteral(", ")), m_failures.isEmpty() ? QString() : m_failures.last());
        finish(result);
        return;
    }

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, [this] { m_timer.start(m_readTimeout); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] {
        if (m_socket)
            failServer(m_socket->errorString());
    });
    connect(m_socket, &QTcpSocket::readyRead, this, [this] {
        m_reply += m_socket->readAll();
        if (m_reply.size() > maxReply) {
            failServer(tr("reply too long"));
            return;
        }
        if (!m_sent) {
            const qsizetype end = m_reply.indexOf('\n');
            if (end < 0)
                return;
            if (!m_reply.startsWith("220")) {
                failServer(QString::fromUtf8(m_reply.left(end)).trimmed());
                return;
            }
            // The word alone: not even the name of this program.
            m_socket->write("DEFINE * " + quoted(m_word) + "\r\nQUIT\r\n");
            m_sent = true;
        }
        // The goodbye, as the last line: a definition may contain "221" at the start of one.
        if (m_reply.endsWith('\n')
            && m_reply.mid(m_reply.lastIndexOf('\n', m_reply.size() - 2) + 1).startsWith("221 ")) {
            const Server &server = m_servers.at(m_index);
            Result result = parseResponse(m_reply, m_word);
            result.server = server.host;
            finish(result);
        }
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        // A server that hung up after answering is fine; before, it is not.
        if (m_sent && (m_reply.contains("\n250 ") || m_reply.contains("\n552 "))) {
            Result result = parseResponse(m_reply, m_word);
            result.server = m_servers.at(m_index).host;
            finish(result);
        } else {
            failServer(tr("connection closed"));
        }
    });
    connect(&m_timer, &QTimer::timeout, m_socket, [this] { failServer(tr("timed out")); });
    m_timer.start(m_connectTimeout);
    m_socket->connectToHost(m_servers.at(index).host, m_servers.at(index).port);
}

void DictClient::failServer(const QString &why)
{
    if (!m_socket)
        return;
    m_failures << m_servers.at(m_index).host + QStringLiteral(": ") + why;
    tryServer(m_index + 1);
}

void DictClient::finish(Result result)
{
    dropSocket();
    if (const auto done = std::exchange(m_done, nullptr))
        done(std::move(result));
}

void DictClient::dropSocket()
{
    m_timer.stop();
    m_timer.disconnect();
    if (!m_socket)
        return;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
}

} // namespace noteAppC
