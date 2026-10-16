#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QTcpSocket;

namespace noteAppC {

/// A DICT client (RFC 2229), enough for "what does this word mean": one
/// `DEFINE * "word"` to the first server that answers, every database it has
/// (on dict.org that is WordNet with its synonyms, a thesaurus, ...).
///
/// The only thing that ever leaves the machine is that command, in plain
/// text, and only when define() is called. The servers are the ones the
/// `dict` command uses: `~/.dictrc`, then `/etc/dict/dict.conf`.
class DictClient : public QObject {
    Q_OBJECT

public:
    struct Server {
        QString host;
        quint16 port = 2628;
        bool operator==(const Server &other) const { return host == other.host && port == other.port; }
    };
    struct Definition {
        QString database, description, text;
    };
    /// `error` empty is success. "No definition found" is an error too.
    struct Result {
        QString word, server, error;
        QList<Definition> definitions;
    };

    explicit DictClient(QObject *parent = nullptr) : QObject(parent) {}

    /// `server HOST` lines, each optionally followed by `{ port N }`; `#` comments.
    static QList<Server> parseConfig(const QString &text);
    /// "host" or "host:port"; an empty host when `spec` is blank.
    static Server parseServerSpec(const QString &spec);
    /// `~/.dictrc` then `/etc/dict/dict.conf`, without repeats; dict.org when
    /// neither names a server.
    static QList<Server> serversFromSystem();
    /// What the servers said, from the `220` banner to the `221`.
    static Result parseResponse(const QByteArray &transcript, const QString &word);
    /// The result as a small note for the popover.
    static QString toMarkdown(const Result &result);

    void setServers(QList<Server> servers) { m_servers = std::move(servers); }
    /// Asks, in the event loop; `done` runs once, on this thread. A new
    /// define() abandons the one before it, whose `done` never runs.
    void define(const QString &word, std::function<void(Result)> done);

private:
    void tryServer(int index);
    /// The server at m_index could not be used; the next one, or give up.
    void failServer(const QString &why);
    void finish(Result result);
    void dropSocket();

    QList<Server> m_servers;
    QTcpSocket *m_socket = nullptr;
    QTimer m_timer;
    QByteArray m_reply;
    std::function<void(Result)> m_done;
    QString m_word;
    QStringList m_failures; ///< "host: why", for the offline message
    int m_index = 0;
    bool m_sent = false;
};

} // namespace noteAppC
