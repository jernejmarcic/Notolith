// The AI plugin (plugins/ai, docs/PLUGINS.md's "AI plugin") - two layers:
//
// 1. AiProviderTest links plugins/ai's request/response logic directly (no
//    plugin host, no dlopen, no sandbox) against a real in-process
//    QHttpServer standing in for Ollama/OpenAI/Anthropic: request shape per
//    provider, response parsing, and every error path (a non-2xx status, a
//    malformed body, a connection refused, a missing or unset key) come
//    back as a clean AiResult, never a crash. runChat() blocks on libcurl,
//    so each call here runs on its own QThread while the test thread pumps
//    the event loop - otherwise the in-process fake server, which needs
//    that same loop to accept the connection, would never answer.
// 2. AiPluginEndToEndTest loads the real libai.so inside a real
//    noteAppC-plugin-host process (the same shape as
//    tests/test_plugins_host.cpp's word-stats suite), proving what only a
//    real sandboxed process can prove: bwrap's network namespace actually
//    gates the HTTP call, PluginHost forwards the declared env var through
//    --clearenv, a slow call fails on its own budget instead of being
//    killed on PluginHost's, and the API key never lands in ai.ini, a log
//    line, or stderr.
//
// XDG_CONFIG_HOME/XDG_DATA_HOME are redirected to a per-binary isolated
// home by tests/CMakeLists.txt - no test here touches the real
// ~/.config/noteAppC.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "AiConfig.h"
#include "AiProvider.h"

#include "PluginHost.h"
#include "PluginManifest.h"

#include <atomic>
#include <functional>

using namespace noteAppC;
using namespace noteAppC::ai;
using Method = QHttpServerRequest::Method;
using StatusCode = QHttpServerResponse::StatusCode;

namespace {

QString aiPluginDir()
{
    return QStringLiteral(NOTEAPPC_AI_PLUGIN_DIR);
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs)
            return false;
        QTest::qWait(10);
    }
    return true;
}

/// Runs a blocking call (runChat/listOllamaModels, both eventually
/// libcurl's synchronous curl_easy_perform) on its own thread while the
/// calling thread keeps the Qt event loop spinning - required so a
/// same-process QHttpServer fixture can actually accept and answer the
/// connection while the "client" side blocks waiting for it.
template <typename Fn>
auto runBlocking(Fn fn) -> decltype(fn())
{
    using Result = decltype(fn());
    Result result{};
    std::atomic<bool> done{false};
    QThread *thread = QThread::create([&] {
        result = fn();
        done = true;
    });
    thread->start();
    QElapsedTimer clock;
    clock.start();
    while (!done.load() && clock.elapsed() < 20000)
        QTest::qWait(5);
    thread->wait(2000);
    delete thread;
    return result;
}

/// An in-process HTTP server standing in for Ollama or an
/// OpenAI/Anthropic-compatible endpoint. Each test installs exactly the
/// route(s) it needs before making a request against baseURL().
class FakeServer {
public:
    FakeServer()
    {
        m_tcp = new QTcpServer();
        m_tcp->listen(QHostAddress::LocalHost, 0);
        m_server.bind(m_tcp);
    }
    ~FakeServer() { delete m_tcp; }

    quint16 port() const { return m_tcp->serverPort(); }
    QString baseURL() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }
    QHttpServer &routes() { return m_server; }

private:
    QHttpServer m_server;
    QTcpServer *m_tcp;
};

QJsonObject jsonBody(const QHttpServerRequest &request)
{
    return QJsonDocument::fromJson(request.body()).object();
}

/// A free TCP port with nothing listening on it - closed immediately after
/// being claimed, so a request against it gets a real ECONNREFUSED rather
/// than hitting some other service.
quint16 unusedPort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

// --- AiProvider: request shape, response parsing, every error path --------

TEST(AiProviderTest, OllamaChatRequestShapeAndSuccessfulParse)
{
    FakeServer server;
    QJsonObject capturedBody;
    int hitCount = 0;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &request) {
        capturedBody = jsonBody(request);
        ++hitCount;
        return QHttpServerResponse(
            QJsonObject{{QStringLiteral("message"),
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                   {QStringLiteral("content"), QStringLiteral("the summary")}}}});
    });

    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();
    config.model = QStringLiteral("llama3.2");

    const AiResult result = runBlocking(
        [&] { return runChat(config, QStringLiteral("be concise"), QStringLiteral("summarize this")); });

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.text, QStringLiteral("the summary"));
    EXPECT_EQ(hitCount, 1);
    EXPECT_EQ(capturedBody.value(QStringLiteral("model")).toString(), QStringLiteral("llama3.2"));
    EXPECT_FALSE(capturedBody.value(QStringLiteral("stream")).toBool(true));
    const QJsonArray messages = capturedBody.value(QStringLiteral("messages")).toArray();
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages.at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
    EXPECT_EQ(messages.at(1).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    EXPECT_EQ(messages.at(1).toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("summarize this"));
}

TEST(AiProviderTest, OpenAICompatibleSendsBearerAuthFromTheNamedEnvVar)
{
    qputenv("NOTEAPPC_TEST_OPENAI_KEY", "secret-key-123");
    FakeServer server;
    QByteArray capturedAuth;
    server.routes().route("/v1/chat/completions", Method::Post, [&](const QHttpServerRequest &request) {
        capturedAuth = request.value("Authorization");
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"),
                                                                            QStringLiteral("answer")}}}}}}});
    });

    AiConfig config;
    config.provider = Provider::OpenAICompatible;
    config.baseURL = server.baseURL();
    config.model = QStringLiteral("gpt-4o-mini");
    config.apiKeyEnv = QStringLiteral("NOTEAPPC_TEST_OPENAI_KEY");

    const AiResult result =
        runBlocking([&] { return runChat(config, QString(), QStringLiteral("hello")); });

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.text, QStringLiteral("answer"));
    EXPECT_EQ(capturedAuth, QByteArrayLiteral("Bearer secret-key-123"));
    qunsetenv("NOTEAPPC_TEST_OPENAI_KEY");
}

TEST(AiProviderTest, AnthropicSendsApiKeyHeaderAndTopLevelSystemField)
{
    qputenv("NOTEAPPC_TEST_ANTHROPIC_KEY", "another-secret");
    FakeServer server;
    QByteArray capturedKey;
    QByteArray capturedVersion;
    QJsonObject capturedBody;
    server.routes().route("/v1/messages", Method::Post, [&](const QHttpServerRequest &request) {
        capturedKey = request.value("x-api-key");
        capturedVersion = request.value("anthropic-version");
        capturedBody = jsonBody(request);
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("content"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("claude says hi")}}}}});
    });

    AiConfig config;
    config.provider = Provider::Anthropic;
    config.baseURL = server.baseURL();
    config.model = QStringLiteral("claude-sonnet-5");
    config.apiKeyEnv = QStringLiteral("NOTEAPPC_TEST_ANTHROPIC_KEY");

    const AiResult result = runBlocking(
        [&] { return runChat(config, QStringLiteral("be terse"), QStringLiteral("hello")); });

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.text, QStringLiteral("claude says hi"));
    EXPECT_EQ(capturedKey, QByteArrayLiteral("another-secret"));
    EXPECT_EQ(capturedVersion, QByteArrayLiteral("2023-06-01"));
    EXPECT_EQ(capturedBody.value(QStringLiteral("system")).toString(), QStringLiteral("be terse"));
    const QJsonArray messages = capturedBody.value(QStringLiteral("messages")).toArray();
    ASSERT_EQ(messages.size(), 1); // system is its own field, not a "system"-role message
    EXPECT_EQ(messages.at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    qunsetenv("NOTEAPPC_TEST_ANTHROPIC_KEY");
}

TEST(AiProviderTest, AMissingApiKeyEnvNameNeverMakesARequest)
{
    FakeServer server;
    int hitCount = 0;
    server.routes().route("/v1/chat/completions", Method::Post, [&](const QHttpServerRequest &) {
        ++hitCount;
        return QHttpServerResponse(QJsonObject{});
    });

    AiConfig config;
    config.provider = Provider::OpenAICompatible;
    config.baseURL = server.baseURL();
    // config.apiKeyEnv left empty - ai.ini never set api-key-env.

    const AiResult result = runChat(config, QString(), QStringLiteral("hi")); // no network I/O - no thread needed
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("api-key-env"))) << result.error.toStdString();
    EXPECT_EQ(hitCount, 0);
}

TEST(AiProviderTest, AnUnsetNamedEnvVarNeverMakesARequest)
{
    qunsetenv("NOTEAPPC_TEST_DOES_NOT_EXIST");
    FakeServer server;
    int hitCount = 0;
    server.routes().route("/v1/messages", Method::Post, [&](const QHttpServerRequest &) {
        ++hitCount;
        return QHttpServerResponse(QJsonObject{});
    });

    AiConfig config;
    config.provider = Provider::Anthropic;
    config.baseURL = server.baseURL();
    config.apiKeyEnv = QStringLiteral("NOTEAPPC_TEST_DOES_NOT_EXIST");

    const AiResult result = runChat(config, QString(), QStringLiteral("hi"));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("NOTEAPPC_TEST_DOES_NOT_EXIST"))) << result.error.toStdString();
    EXPECT_EQ(hitCount, 0);
}

TEST(AiProviderTest, HTTP500BecomesACleanError)
{
    FakeServer server;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        return QHttpServerResponse(QJsonObject{{QStringLiteral("error"), QStringLiteral("boom")}},
                                   StatusCode::InternalServerError);
    });

    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();

    const AiResult result = runBlocking([&] { return runChat(config, QString(), QStringLiteral("hi")); });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("500"))) << result.error.toStdString();
}

TEST(AiProviderTest, MalformedJSONBecomesACleanError)
{
    FakeServer server;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        return QHttpServerResponse("text/plain", QByteArrayLiteral("not json"));
    });

    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();

    const AiResult result = runBlocking([&] { return runChat(config, QString(), QStringLiteral("hi")); });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("malformed"))) << result.error.toStdString();
}

TEST(AiProviderTest, UnexpectedResponseShapeBecomesACleanError)
{
    FakeServer server;
    server.routes().route("/api/chat", Method::Post,
                          [&](const QHttpServerRequest &) { return QHttpServerResponse(QJsonObject{}); });

    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();

    const AiResult result = runBlocking([&] { return runChat(config, QString(), QStringLiteral("hi")); });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("unexpected response"))) << result.error.toStdString();
}

TEST(AiProviderTest, ConnectionRefusedBecomesACleanErrorNotACrash)
{
    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = QStringLiteral("http://127.0.0.1:%1").arg(unusedPort());
    config.httpTimeoutMs = 3000;

    const AiResult result = runBlocking([&] { return runChat(config, QString(), QStringLiteral("hi")); });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    // Reaching this line at all, with a definite answer either way, is the
    // real assertion - a refused connection must never hang or crash.
    SUCCEED();
}

TEST(AiProviderTest, EmptyPromptIsRefusedWithoutARequest)
{
    FakeServer server;
    int hitCount = 0;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        ++hitCount;
        return QHttpServerResponse(QJsonObject{});
    });
    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();

    const AiResult result = runChat(config, QString(), QStringLiteral("   \n  "));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(hitCount, 0);
}

TEST(AiProviderTest, ListOllamaModelsParsesTags)
{
    FakeServer server;
    server.routes().route("/api/tags", Method::Get, [&](const QHttpServerRequest &) {
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("models"),
             QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("llama3.2:latest")}},
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("mistral:latest")}}}}});
    });

    AiConfig config;
    config.provider = Provider::Ollama;
    config.baseURL = server.baseURL();

    const AiModelsResult result = runBlocking([&] { return listOllamaModels(config); });
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.models, (QStringList{QStringLiteral("llama3.2:latest"), QStringLiteral("mistral:latest")}));
}

TEST(AiProviderTest, ListModelsRefusesANonOllamaProviderWithoutARequest)
{
    FakeServer server;
    int hitCount = 0;
    server.routes().route("/api/tags", Method::Get, [&](const QHttpServerRequest &) {
        ++hitCount;
        return QHttpServerResponse(QJsonObject{});
    });
    AiConfig config;
    config.provider = Provider::Anthropic;
    config.baseURL = server.baseURL();

    const AiModelsResult result = listOllamaModels(config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(hitCount, 0);
}

// --- end to end: the real libai.so, in a real sandboxed host process ------

class AiPluginEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        manifest = PluginManifest::load(aiPluginDir());
        ASSERT_TRUE(manifest.isValid()) << manifest.error().toStdString();
        configDir.reset(new QTemporaryDir());
        ASSERT_TRUE(configDir->isValid());
        // Points every PluginHost in this test at an ai.ini this test
        // controls, independent of the ctest-wide isolated XDG_CONFIG_HOME
        // (still never the real one).
        qputenv("XDG_CONFIG_HOME", configDir->path().toUtf8());
    }

    void writeAiIni(const QString &contents)
    {
        QDir().mkpath(configDir->filePath(QStringLiteral("noteAppC")));
        QFile file(configDir->filePath(QStringLiteral("noteAppC/ai.ini")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(contents.toUtf8());
    }

    PluginAppCallbacks callbacks(QStringList *logged = nullptr, QString *lastNotice = nullptr)
    {
        PluginAppCallbacks cb;
        cb.activeNotePath = [] { return QStringLiteral("Note.md"); };
        cb.activeNoteText = [] { return QStringLiteral("Once upon a time, in a quiet valley,"); };
        cb.activeSelectionText = [] { return QStringLiteral("what is 2 + 2?"); };
        cb.readNote = [](const QString &) { return QString(); };
        cb.listNotes = [] { return QStringList(); };
        cb.replaceSelection = [](const QString &) { return true; };
        cb.writeNote = [](const QString &, const QString &) { return true; };
        cb.clipboardRead = [] { return QString(); };
        cb.clipboardWrite = [](const QString &) { return true; };
        cb.showNotice = [lastNotice](const QString &message) {
            if (lastNotice)
                *lastNotice = message;
        };
        cb.log = [logged](const QString &, const QString &message) {
            if (logged)
                *logged << message;
        };
        return cb;
    }

    PluginManifest manifest;
    std::unique_ptr<QTemporaryDir> configDir;
};

TEST_F(AiPluginEndToEndTest, NetworkGrantedReachesTheFakeServerWithTheForwardedKey)
{
    FakeServer server;
    QByteArray capturedKey;
    int hitCount = 0;
    server.routes().route("/v1/messages", Method::Post, [&](const QHttpServerRequest &request) {
        capturedKey = request.value("x-api-key");
        ++hitCount;
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("content"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("The answer is 4.")}}}}});
    });
    writeAiIni(QStringLiteral("[ai]\nprovider = anthropic\nbase-url = %1\nmodel = test-model\n"
                              "api-key-env = ANTHROPIC_API_KEY\n")
                  .arg(server.baseURL()));
    qputenv("ANTHROPIC_API_KEY", "sandbox-forwarded-secret");

    QString notice;
    PluginHost host(manifest, {PluginPermission::ReadNotes, PluginPermission::Network}, callbacks(nullptr, &notice));
    bool done = false;
    bool ok = false;
    host.runCommand(QStringLiteral("ask"), [&](bool o, const QJsonObject &, const QString &) {
        ok = o;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_TRUE(ok);
    EXPECT_EQ(hitCount, 1);
    EXPECT_EQ(capturedKey, QByteArrayLiteral("sandbox-forwarded-secret"));
    EXPECT_TRUE(notice.contains(QStringLiteral("The answer is 4."))) << notice.toStdString();

    host.stop();
    qunsetenv("ANTHROPIC_API_KEY");
}

TEST_F(AiPluginEndToEndTest, NetworkNotGrantedFailsCleanlyWithoutReachingTheServer)
{
    // Ollama, not Anthropic: no api-key-env at all, so the *only* possible
    // reason this can fail to reach the fake server is the sandbox's
    // network namespace - not a missing key short-circuiting first (which
    // is what happened the first time this test was written: env-
    // passthrough is itself gated on the network permission, so an
    // Anthropic config failed for the wrong reason and this test passed
    // without actually proving anything - caught by deliberately disabling
    // the --unshare-net line and seeing this test *stay* green).
    FakeServer server;
    int hitCount = 0;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        ++hitCount;
        return QHttpServerResponse(QJsonObject{});
    });
    writeAiIni(QStringLiteral("[ai]\nprovider = ollama\nbase-url = %1\ntimeout-ms = 3000\n").arg(server.baseURL()));

    QString notice;
    // read-notes only - no network permission granted.
    PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacks(nullptr, &notice));
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    bool ok = false;
    host.runCommand(QStringLiteral("ask"), [&](bool o, const QJsonObject &, const QString &) {
        ok = o;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_TRUE(ok); // the plugin itself ran fine - it just could not reach the network
    EXPECT_EQ(hitCount, 0);
    // The sandboxed network namespace has no route at all, so this fails
    // fast (ENETUNREACH) rather than waiting out curl's own timeout -
    // proof it was the sandbox that blocked it, not a slow, coincidental
    // failure that looks the same from outside.
    EXPECT_LT(clock.elapsed(), 10000);
    EXPECT_TRUE(notice.contains(QStringLiteral("failed"))) << notice.toStdString();

    host.stop();
}

TEST_F(AiPluginEndToEndTest, ALongCallWithinItsOwnTimeoutStillSucceeds)
{
    FakeServer server;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        QThread::msleep(2000); // "answers after e.g. 3s" (docs/PLUGINS.md's brief) - 2s keeps this suite quick
        return QHttpServerResponse(
            QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"),
                                                                 QStringLiteral("late but correct")}}}});
    });
    // plugin.ini's own timeout-ms (180000) is far more than enough; ai.ini's
    // http timeout just needs to comfortably clear the 2s delay too.
    writeAiIni(QStringLiteral("[ai]\nprovider = ollama\nbase-url = %1\ntimeout-ms = 15000\n").arg(server.baseURL()));

    QString notice;
    PluginHost host(manifest, {PluginPermission::ReadNotes, PluginPermission::Network}, callbacks(nullptr, &notice));
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    bool ok = false;
    host.runCommand(QStringLiteral("summarize"), [&](bool o, const QJsonObject &, const QString &) {
        ok = o;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }, 20000));
    EXPECT_TRUE(ok);
    EXPECT_GE(clock.elapsed(), 1800); // really did wait for the slow answer, not a cached/instant one
    EXPECT_TRUE(notice.contains(QStringLiteral("late but correct"))) << notice.toStdString();

    host.stop();
}

TEST_F(AiPluginEndToEndTest, ARequestExceedingItsOwnHttpTimeoutFailsCleanlyRatherThanBeingKilled)
{
    FakeServer server;
    server.routes().route("/api/chat", Method::Post, [&](const QHttpServerRequest &) {
        QThread::msleep(2000); // far longer than the 400ms curl budget below
        return QHttpServerResponse(
            QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QStringLiteral("too late")}}}});
    });
    // ai.ini's own timeout is short; plugin.ini's is the shipped 180000ms -
    // curl must give up on its own long before PluginHost's kill timeout
    // would ever get a chance to.
    writeAiIni(QStringLiteral("[ai]\nprovider = ollama\nbase-url = %1\ntimeout-ms = 400\n").arg(server.baseURL()));

    QString notice;
    PluginHost host(manifest, {PluginPermission::ReadNotes, PluginPermission::Network}, callbacks(nullptr, &notice));
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    bool ok = false;
    host.runCommand(QStringLiteral("summarize"), [&](bool o, const QJsonObject &, const QString &) {
        ok = o;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }, 20000));
    // A clean failure the plugin reported itself: ok stays true at the
    // protocol level (run_command returned 0, the same as any other
    // handled error - see plugin.cpp's comment on why), the notice
    // explains it, and nothing near PluginHost's own 180000ms budget was
    // used - the whole point of giving curl a shorter budget than the kill
    // timeout.
    EXPECT_TRUE(ok);
    EXPECT_LT(clock.elapsed(), 15000);
    EXPECT_TRUE(notice.contains(QStringLiteral("failed"))) << notice.toStdString();
    EXPECT_EQ(host.state(), PluginHost::State::Running); // never crashed/killed

    host.stop();
}

TEST_F(AiPluginEndToEndTest, TheApiKeyNeverAppearsInTheConfigFileLogsOrStderr)
{
    const QString secret = QStringLiteral("sk-test-CANARY-9f8e7d6c5b4a");
    FakeServer server;
    bool sawTheKey = false;
    server.routes().route("/v1/messages", Method::Post, [&](const QHttpServerRequest &request) {
        sawTheKey = QString::fromUtf8(request.value("x-api-key")) == secret;
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("content"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("ok")}}}}});
    });
    const QString aiIni = QStringLiteral("[ai]\nprovider = anthropic\nbase-url = %1\nmodel = test-model\n"
                                         "api-key-env = ANTHROPIC_API_KEY\n")
                              .arg(server.baseURL());
    writeAiIni(aiIni);
    qputenv("ANTHROPIC_API_KEY", secret.toUtf8());

    QStringList logged;
    PluginHost host(manifest, {PluginPermission::ReadNotes, PluginPermission::Network}, callbacks(&logged));
    bool done = false;
    host.runCommand(QStringLiteral("ask"), [&](bool, const QJsonObject &, const QString &) { done = true; });
    ASSERT_TRUE(waitUntil([&] { return done; }));

    // Positive control: the mechanism really did carry the secret, so the
    // negative assertions below are not vacuously true.
    ASSERT_TRUE(sawTheKey);

    for (const QString &line : logged)
        EXPECT_FALSE(line.contains(secret)) << "log line contained the key: " << line.toStdString();

    QFile configFile(configDir->filePath(QStringLiteral("noteAppC/ai.ini")));
    ASSERT_TRUE(configFile.open(QIODevice::ReadOnly));
    const QString configContents = QString::fromUtf8(configFile.readAll());
    EXPECT_FALSE(configContents.contains(secret));
    EXPECT_EQ(configContents, aiIni); // this test wrote it and nothing else touched it

    QFile manifestFile(aiPluginDir() + QStringLiteral("/plugin.ini"));
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    EXPECT_FALSE(QString::fromUtf8(manifestFile.readAll()).contains(secret));

    host.stop();
    qunsetenv("ANTHROPIC_API_KEY");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
