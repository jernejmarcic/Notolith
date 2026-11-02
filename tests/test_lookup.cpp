// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include "DictClient.h"

using noteAppC::DictClient;

namespace {

const QByteArray reply =
    "150 2 definitions retrieved\r\n"
    "151 \"hello\" wn \"WordNet (r) 3.0 (2006)\"\r\n"
    "hello\r\n"
    "  n 1: an expression of greeting [syn: {hello}, {hullo}]\r\n"
    "..leading dot\r\n"
    ".\r\n"
    "151 \"hello\" moby-thesaurus \"Moby Thesaurus II by Grady Ward\"\r\n"
    "hello\r\n"
    "  5 synonyms: hi, hey\r\n"
    ".\r\n"
    "250 ok [d/m/c = 2/0/21]\r\n"
    "221 bye [d/m/c = 0/0/0; 0.000r 0.000u 0.000s]\r\n";

/// A DICT server on a loopback port that answers any DEFINE with `reply`
/// and remembers every byte it was sent. Tests never leave this machine.
class FakeServer {
public:
    FakeServer(const QByteArray &answer = reply) : m_answer(answer)
    {
        EXPECT_TRUE(m_server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->write("220 fake dictd ready\r\n");
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                m_received += socket->readAll();
                if (m_received.endsWith("QUIT\r\n")) {
                    socket->write(m_answer);
                    socket->disconnectFromHost();
                }
            });
        });
    }
    DictClient::Server server() const { return {QStringLiteral("127.0.0.1"), m_server.serverPort()}; }
    QByteArray received() const { return m_received; }

private:
    QTcpServer m_server;
    QByteArray m_answer;
    QByteArray m_received;
};

/// A loopback port nothing listens on: the connection is refused at once.
DictClient::Server closedPort(const QString &host = QStringLiteral("127.0.0.1"))
{
    QTcpServer probe;
    EXPECT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    return {host, probe.serverPort()};
}

DictClient::Result define(DictClient &client, const QString &word, bool *finished = nullptr)
{
    DictClient::Result result;
    bool done = false;
    client.define(word, [&](DictClient::Result r) {
        result = std::move(r);
        done = true;
    });
    EXPECT_TRUE(QTest::qWaitFor([&] { return done; }, 3000));
    if (finished)
        *finished = done;
    return result;
}

} // namespace

TEST(DictClientTest, ParsesDictConf)
{
    const auto servers = DictClient::parseConfig(
        QStringLiteral("# the servers\nserver localhost\nserver dict.org { port 2629 }  # remote\n"));
    ASSERT_EQ(servers.size(), 2);
    EXPECT_EQ(servers[0].host, QStringLiteral("localhost"));
    EXPECT_EQ(servers[0].port, 2628);
    EXPECT_EQ(servers[1].host, QStringLiteral("dict.org"));
    EXPECT_EQ(servers[1].port, 2629);

    EXPECT_EQ(DictClient::parseServerSpec(QStringLiteral("dict.example:99")).port, 99);
    EXPECT_EQ(DictClient::parseServerSpec(QStringLiteral(" dict.example ")).host, QStringLiteral("dict.example"));
    EXPECT_EQ(DictClient::parseServerSpec(QStringLiteral("dict.example")).port, 2628);
}

TEST(DictClientTest, ParsesADefineTranscript)
{
    const auto result = DictClient::parseResponse("220 hi\r\n" + reply, QStringLiteral("hello"));
    EXPECT_TRUE(result.error.isEmpty());
    ASSERT_EQ(result.definitions.size(), 2);
    EXPECT_EQ(result.definitions[0].database, QStringLiteral("wn"));
    EXPECT_EQ(result.definitions[0].description, QStringLiteral("WordNet (r) 3.0 (2006)"));
    EXPECT_TRUE(result.definitions[0].text.contains(QStringLiteral("[syn: {hello}, {hullo}]")));
    EXPECT_TRUE(result.definitions[0].text.contains(QStringLiteral("\n.leading dot\n"))) << "dot-stuffing undone";
    EXPECT_EQ(result.definitions[1].database, QStringLiteral("moby-thesaurus"));

    const QString markdown = DictClient::toMarkdown(result);
    EXPECT_TRUE(markdown.startsWith(QStringLiteral("## hello")));
    EXPECT_TRUE(markdown.contains(QStringLiteral("**WordNet (r) 3.0 (2006)**")));
    EXPECT_FALSE(markdown.contains(QStringLiteral("\n  n 1"))) << "indented four spaces would be a code block";
}

TEST(DictClientTest, NoMatchIsAnError)
{
    const auto result =
        DictClient::parseResponse("220 hi\r\n552 no match\r\n221 bye\r\n", QStringLiteral("zzzz"));
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.definitions.isEmpty());
    EXPECT_TRUE(DictClient::toMarkdown(result).contains(QStringLiteral("zzzz")));
}

TEST(DictClientTest, TalksToAFakeServerAndSendsNothingElse)
{
    FakeServer fake;
    DictClient client;
    client.setServers({fake.server()});
    const auto result = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    EXPECT_EQ(result.definitions.size(), 2);
    // The whole disclosure: the word, quoted, and goodbye. Not the program, not the vault.
    EXPECT_EQ(fake.received(), QByteArray("DEFINE * \"hello\"\r\nQUIT\r\n"));
}

TEST(DictClientTest, AWordCannotSmuggleACommand)
{
    FakeServer fake;
    DictClient client;
    client.setServers({fake.server()});
    define(client, QStringLiteral("a\"b\r\nSHOW DB"));
    EXPECT_EQ(fake.received(), QByteArray("DEFINE * \"a\\\"b  SHOW DB\"\r\nQUIT\r\n"));
}

TEST(DictClientTest, FallsThroughARefusedServer)
{
    FakeServer fake;
    DictClient client;
    client.setServers({closedPort(), fake.server()});
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    EXPECT_LT(elapsed.elapsed(), 1000);
}

TEST(DictClientTest, AllServersDownReportsOffline)
{
    DictClient client;
    client.setServers({closedPort(QStringLiteral("127.0.0.1")), closedPort(QStringLiteral("localhost"))});
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(result.definitions.isEmpty());
    EXPECT_TRUE(result.error.contains(QStringLiteral("127.0.0.1"))) << result.error.toStdString();
    EXPECT_TRUE(result.error.contains(QStringLiteral("localhost"))) << result.error.toStdString();
    EXPECT_LT(elapsed.elapsed(), 500);
}

TEST(DictClientTest, ASilentServerTimesOutAndFallsThrough)
{
    // Accepts the connection and never says a word: the connected-but-dead path.
    QTcpServer silent;
    ASSERT_TRUE(silent.listen(QHostAddress::LocalHost, 0));
    std::vector<QTcpSocket *> held;
    QObject::connect(&silent, &QTcpServer::newConnection, &silent,
                     [&] { held.push_back(silent.nextPendingConnection()); });

    FakeServer fake;
    DictClient client;
    client.setTimeouts(300, 300);
    client.setServers({{QStringLiteral("127.0.0.1"), silent.serverPort()}, fake.server()});
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    EXPECT_EQ(held.size(), size_t(1));
    EXPECT_LT(elapsed.elapsed(), 1000);

    client.setServers({{QStringLiteral("127.0.0.1"), silent.serverPort()}});
    const auto alone = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(alone.error.contains(QStringLiteral("timed out"))) << alone.error.toStdString();
}

TEST(DictClientTest, AnEndlessReplyIsCutOff)
{
    // 2 MiB of one "definition" that never ends: well past what an entry is.
    FakeServer fake("151 \"hello\" wn \"x\"\r\n" + QByteArray(2 << 20, 'a'));
    DictClient client;
    client.setServers({fake.server()});
    const auto result = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(result.error.contains(QStringLiteral("reply too long"))) << result.error.toStdString();
    EXPECT_TRUE(result.definitions.isEmpty());
}

TEST(DictClientTest, ANewLookupReplacesAnOldOne)
{
    FakeServer fake;
    DictClient client;
    client.setServers({fake.server()});
    int first = 0;
    client.define(QStringLiteral("one"), [&first](DictClient::Result) { ++first; });
    const auto second = define(client, QStringLiteral("hello"));
    EXPECT_TRUE(second.error.isEmpty());
    QTest::qWait(50);
    EXPECT_EQ(first, 0) << "the abandoned one never reports";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
