// Runtime plugin host (docs/PLUGINS.md, Tier 2): protocol framing on its
// own, then a real noteAppC-plugin-host process loading the word-stats
// example/fixture plugin end to end - permission enforcement, a hang killed
// by the per-request timeout and restarted, a crash that does not take this
// test process down, the sandbox actually denying a file outside the
// plugin's own directory, and the community-plugins feature switch reading
// nothing when it is off.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "PluginHost.h"
#include "PluginManifest.h"
#include "PluginProtocol.h"
#include "PluginRegistry.h"

#include <noteappc/plugin.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>

using namespace noteAppC;

namespace {

QString wordStatsDir()
{
    return QStringLiteral(NOTEAPPC_WORD_STATS_PLUGIN_DIR);
}

/// Spins the event loop until `predicate` is true or `timeoutMs` elapses -
/// every async PluginHost call in this suite goes through it, since a
/// result lands on the event loop (QProcess's signals) at an unpredictable
/// time, the same reason test_window.cpp polls rather than sleeping a fixed
/// amount.
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000)
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

PluginAppCallbacks callbacksReturning(const QString &activeText, QString *lastNotice = nullptr)
{
    PluginAppCallbacks callbacks;
    callbacks.activeNotePath = [] { return QStringLiteral("Note.md"); };
    callbacks.activeNoteText = [activeText] { return activeText; };
    callbacks.activeSelectionText = [] { return QString(); };
    callbacks.readNote = [](const QString &) { return QString(); };
    callbacks.listNotes = [] { return QStringList(); };
    callbacks.replaceSelection = [](const QString &) { return true; };
    callbacks.writeNote = [](const QString &, const QString &) { return true; };
    callbacks.clipboardRead = [] { return QString(); };
    callbacks.clipboardWrite = [](const QString &) { return true; };
    callbacks.showNotice = [lastNotice](const QString &message) {
        if (lastNotice)
            *lastNotice = message;
    };
    callbacks.log = [](const QString &, const QString &) {};
    return callbacks;
}

QByteArray bigEndianHeader(quint32 length)
{
    QByteArray header;
    header.append(char((length >> 24) & 0xff));
    header.append(char((length >> 16) & 0xff));
    header.append(char((length >> 8) & 0xff));
    header.append(char(length & 0xff));
    return header;
}

} // namespace

// --- framing: PluginProtocol on its own, no process involved ---------------

TEST(PluginManifestTest, ListsWithSeveralItemsKeepEveryItem)
{
    const PluginManifest manifest = PluginManifest::load(QStringLiteral(NOTEAPPC_WORD_STATS_PLUGIN_DIR));
    ASSERT_TRUE(manifest.isValid()) << manifest.error().toStdString();
    EXPECT_EQ(manifest.commands(),
              (QStringList{QStringLiteral("count-words"), QStringLiteral("hang"), QStringLiteral("crash")}));
    EXPECT_EQ(manifest.codeBlockLanguages(), QStringList{QStringLiteral("calc")});
}

TEST(PluginManifestTest, ParsesCommandTitlesTimeoutMsAndEnvPassthrough)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile ini(dir.filePath(QStringLiteral("plugin.ini")));
    ASSERT_TRUE(ini.open(QIODevice::WriteOnly));
    ini.write(QByteArrayLiteral(
        "[plugin]\n"
        "id = titled\n"
        "library = libtitled.so\n"
        "timeout-ms = 250000\n"
        "env-passthrough = ANTHROPIC_API_KEY, OPENAI_API_KEY\n"
        "[provides]\n"
        "commands = summarize\n"
        "[commands]\n"
        "summarize = Summarize this note\n"));
    ini.close();

    const PluginManifest manifest = PluginManifest::load(dir.path());
    ASSERT_TRUE(manifest.isValid()) << manifest.error().toStdString();
    EXPECT_EQ(manifest.commandTitle(QStringLiteral("summarize")), QStringLiteral("Summarize this note"));
    EXPECT_TRUE(manifest.commandTitle(QStringLiteral("no-such-command")).isEmpty());
    EXPECT_EQ(manifest.requestTimeoutMs(), 250000);
    EXPECT_EQ(manifest.envPassthrough(),
              (QStringList{QStringLiteral("ANTHROPIC_API_KEY"), QStringLiteral("OPENAI_API_KEY")}));
}

TEST(PluginManifestTest, TimeoutMsIsClampedAndZeroMeansUseTheDefault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile ini(dir.filePath(QStringLiteral("plugin.ini")));
    ASSERT_TRUE(ini.open(QIODevice::WriteOnly));
    ini.write(QByteArrayLiteral("[plugin]\nid = clamped\nlibrary = libclamped.so\ntimeout-ms = 999999999\n"));
    ini.close();
    EXPECT_EQ(PluginManifest::load(dir.path()).requestTimeoutMs(), 300000);

    QTemporaryDir dirUnset;
    ASSERT_TRUE(dirUnset.isValid());
    QFile iniUnset(dirUnset.filePath(QStringLiteral("plugin.ini")));
    ASSERT_TRUE(iniUnset.open(QIODevice::WriteOnly));
    iniUnset.write(QByteArrayLiteral("[plugin]\nid = unset\nlibrary = libunset.so\n"));
    iniUnset.close();
    EXPECT_EQ(PluginManifest::load(dirUnset.path()).requestTimeoutMs(), 0);
}

TEST(PluginProtocolFraming, RoundTripsEveryMessageKind)
{
    QJsonObject params;
    params.insert(QStringLiteral("commandID"), QStringLiteral("count-words"));
    const PluginMessage request = PluginMessage::request(7, QStringLiteral("command.run"), params);
    PluginFrameReader reader;
    reader.feed(plugin_protocol_encode(request));
    PluginMessage decoded;
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(decoded.kind, PluginMessage::Kind::Request);
    EXPECT_EQ(decoded.id, 7);
    EXPECT_EQ(decoded.method, QStringLiteral("command.run"));
    EXPECT_EQ(decoded.params.value(QStringLiteral("commandID")).toString(), QStringLiteral("count-words"));

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    reader.feed(plugin_protocol_encode(PluginMessage::response(7, result)));
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(decoded.kind, PluginMessage::Kind::Response);
    EXPECT_TRUE(decoded.ok);

    reader.feed(plugin_protocol_encode(
        PluginMessage::errorResponse(8, QStringLiteral("permission-denied"), QStringLiteral("nope"))));
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_FALSE(decoded.ok);
    EXPECT_EQ(decoded.errorCode, QStringLiteral("permission-denied"));

    QJsonObject eventParams;
    eventParams.insert(QStringLiteral("event"), QStringLiteral("note-opened"));
    reader.feed(plugin_protocol_encode(PluginMessage::notification(QStringLiteral("vault.event"), eventParams)));
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(decoded.kind, PluginMessage::Kind::Notification);
    EXPECT_EQ(decoded.params.value(QStringLiteral("event")).toString(), QStringLiteral("note-opened"));
}

TEST(PluginProtocolFraming, PartialReadsWaitForTheWholeFrame)
{
    const QByteArray frame = plugin_protocol_encode(PluginMessage::notification(QStringLiteral("log"), {}));
    ASSERT_GT(frame.size(), 6);
    PluginFrameReader reader;
    PluginMessage decoded;

    reader.feed(frame.left(2)); // not even the full length prefix yet
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::NeedMoreData);
    reader.feed(frame.mid(2, 3)); // the rest of the prefix plus one payload byte
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::NeedMoreData);
    reader.feed(frame.mid(5)); // the rest of the payload
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(decoded.method, QStringLiteral("log"));

    // Two whole frames fed at once - the second must still be there after
    // the first is popped, proving the reader stops exactly at one frame.
    reader.feed(frame + frame);
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::NeedMoreData);
}

TEST(PluginProtocolFraming, OversizedFrameIsRejected)
{
    PluginFrameReader reader;
    reader.feed(bigEndianHeader(static_cast<quint32>(PluginFrameReader::maxFrameSize()) + 1));
    PluginMessage decoded;
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::FrameTooLarge);
    // The reader gave up on the stream rather than waiting for the 32 MiB a
    // frame that size would need before it could even be parsed - proven by
    // feeding fewer than 4 bytes (not even one full length prefix): if the
    // oversized header were still sitting in the buffer this would parse as
    // part of a length prefix instead of legitimately needing more data.
    reader.feed(QByteArray(2, '\0'));
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::NeedMoreData);
}

TEST(PluginProtocolFraming, MalformedJSONIsRejectedAndTheStreamResyncs)
{
    PluginFrameReader reader;
    const QByteArray payload = "not json";
    reader.feed(bigEndianHeader(static_cast<quint32>(payload.size())) + payload);
    reader.feed(plugin_protocol_encode(PluginMessage::notification(QStringLiteral("log"), {})));

    PluginMessage decoded;
    EXPECT_EQ(reader.next(&decoded), PluginFrameReader::Status::MalformedJSON);
    ASSERT_EQ(reader.next(&decoded), PluginFrameReader::Status::Ok);
    EXPECT_EQ(decoded.method, QStringLiteral("log"));
}

// --- the header-only C++ wrapper (plugin.hpp), compiled but never dlopen'd here --

namespace {
noteAppC::Plugin gWrapperTestPlugin;
int wrapperTestInit()
{
    gWrapperTestPlugin.onCommand("ping", [] { return 0; });
    gWrapperTestPlugin.onCodeBlock("echo", [](const std::string &source, int, float) {
        return noteAppC::BlockResult::ofText(source);
    });
    return 0;
}
} // namespace
NOTEAPPC_PLUGIN_ENTRY(gWrapperTestPlugin, wrapperTestInit)

TEST(PluginHeaderOnlyWrapper, DispatchesCommandsAndCodeBlocksThroughTheCABI)
{
    const noteappc_plugin *table = noteappc_plugin_entry();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->api, static_cast<uint32_t>(NOTEAPPC_PLUGIN_API));
    noteappc_host host{};
    host.api = NOTEAPPC_PLUGIN_API;
    ASSERT_EQ(table->init(&host), 0);
    EXPECT_EQ(table->run_command("ping"), 0);
    EXPECT_EQ(table->run_command("not-registered"), 1);

    noteappc_block_result result{};
    ASSERT_EQ(table->render_code_block("echo", "hello", 100, 1.0f, &result), 0);
    ASSERT_NE(result.text, nullptr);
    EXPECT_STREQ(result.text, "hello");
    std::free(result.text);
    noteappc_block_result missing{};
    EXPECT_EQ(table->render_code_block("no-such-language", "x", 100, 1.0f, &missing), 1);
}

// --- end to end through a real noteAppC-plugin-host process ----------------

class PluginHostTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        manifest = PluginManifest::load(wordStatsDir());
        ASSERT_TRUE(manifest.isValid()) << manifest.error().toStdString();
    }
    PluginManifest manifest;
};

TEST_F(PluginHostTest, WithoutBubblewrapAPluginRefusesToStart)
{
    // No bwrap on PATH: the plugin must not run bare, with the whole home
    // directory in reach.
    const QByteArray path = qgetenv("PATH");
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    qputenv("PATH", empty.path().toUtf8());
    PluginHost host(manifest, {PluginPermission::ReadNotes},
                    callbacksReturning(QStringLiteral("three little words"), nullptr));
    bool done = false;
    bool ok = true;
    QString error;
    host.runCommand(QStringLiteral("count-words"), [&](bool success, const QJsonObject &, const QString &why) {
        ok = success;
        error = why;
        done = true;
    });
    qputenv("PATH", path);
    ASSERT_TRUE(done) << "refused at once, not after a timeout";
    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QLatin1String("bubblewrap"))) << error.toStdString();
    EXPECT_EQ(host.state(), PluginHost::State::Crashed);
}

TEST_F(PluginHostTest, RunsACommandAndRendersACodeBlockEndToEnd)
{
    QString notice;
    PluginHost host(manifest, {PluginPermission::ReadNotes},
                    callbacksReturning(QStringLiteral("three little words"), &notice));

    bool commandDone = false;
    bool commandOk = false;
    host.runCommand(QStringLiteral("count-words"), [&](bool ok, const QJsonObject &, const QString &) {
        commandOk = ok;
        commandDone = true;
    });
    ASSERT_TRUE(waitUntil([&] { return commandDone; }));
    EXPECT_TRUE(commandOk);
    EXPECT_EQ(notice, QStringLiteral("3 words"));
    EXPECT_GE(host.timing().lastStartMs, 0);
    EXPECT_GE(host.timing().lastCommandMs, 0);

    bool renderDone = false;
    QJsonObject renderResult;
    host.renderCodeBlock(QStringLiteral("calc"), QStringLiteral("2+2\n3*4"), 200, 1.0, QStringLiteral("dark"),
                         [&](bool ok, const QJsonObject &result, const QString &) {
                             renderResult = result;
                             renderDone = ok;
                         });
    ASSERT_TRUE(waitUntil([&] { return renderDone; }));
    EXPECT_TRUE(renderResult.value(QStringLiteral("isMarkdown")).toBool());
    const QString text = renderResult.value(QStringLiteral("text")).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("2+2 = 4"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("3*4 = 12"))) << text.toStdString();
    EXPECT_GE(host.timing().lastRenderMs, 0);

    host.stop();
}

TEST_F(PluginHostTest, ARequestOutsideGrantedPermissionsIsRefusedAndLogged)
{
    QString notice;
    QStringList logged;
    PluginAppCallbacks callbacks = callbacksReturning(QStringLiteral("irrelevant"), &notice);
    callbacks.log = [&](const QString &, const QString &message) { logged << message; };
    // No permissions granted - count-words needs read-notes for active_note_text().
    PluginHost host(manifest, {}, callbacks);

    QSignalSpy refusals(&host, &PluginHost::permissionRefused);
    bool done = false;
    host.runCommand(QStringLiteral("count-words"), [&](bool, const QJsonObject &, const QString &) { done = true; });
    ASSERT_TRUE(waitUntil([&] { return done; }));

    ASSERT_EQ(refusals.count(), 1);
    EXPECT_EQ(refusals.at(0).at(0).toString(), QStringLiteral("note.activeText"));
    EXPECT_EQ(refusals.at(0).at(1).toString(), QStringLiteral("read-notes"));
    EXPECT_TRUE(std::any_of(logged.begin(), logged.end(),
                            [](const QString &m) { return m.contains(QStringLiteral("not granted")); }));
    // Refusal is transparent to the plugin (empty text, not an error -
    // plugin.h's documented convention) so the command itself still runs.
    EXPECT_EQ(notice, QStringLiteral("0 words"));

    host.stop();
}

TEST_F(PluginHostTest, AHangingCommandIsKilledByItsTimeoutAndTheHostRestartsOnNextUse)
{
    PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacksReturning(QStringLiteral("x")));
    host.setRequestTimeoutMsForTesting(400);

    bool timedOutDone = false;
    bool timedOutOk = true;
    QString timedOutError;
    host.runCommand(QStringLiteral("hang"), [&](bool ok, const QJsonObject &, const QString &error) {
        timedOutOk = ok;
        timedOutError = error;
        timedOutDone = true;
    });
    ASSERT_TRUE(waitUntil([&] { return timedOutDone; }, 5000));
    EXPECT_FALSE(timedOutOk);
    EXPECT_TRUE(timedOutError.contains(QStringLiteral("timed out"))) << timedOutError.toStdString();
    ASSERT_TRUE(waitUntil([&] { return host.state() == PluginHost::State::Crashed; }));

    // Backoff after one failure is 500ms << 1 = 1000ms
    // (PluginHost::ensureStarted) - wait it out, then prove the *next*
    // command actually starts a fresh, working process rather than the
    // plugin staying dead.
    QTest::qWait(1200);
    bool secondDone = false;
    bool secondOk = false;
    host.runCommand(QStringLiteral("count-words"), [&](bool ok, const QJsonObject &, const QString &) {
        secondOk = ok;
        secondDone = true;
    });
    ASSERT_TRUE(waitUntil([&] { return secondDone; }));
    EXPECT_TRUE(secondOk);

    host.stop();
}

TEST(PluginHostManifestTimeoutTest, ADeclaredTimeoutMsOverridesTheDefaultWithoutTheTestingHook)
{
    // A plugin.ini's own `timeout-ms` (docs/PLUGINS.md, "Timeouts") has to
    // take effect from the constructor alone - setRequestTimeoutMsForTesting
    // exists only so a *different* test does not need to wait out a real
    // one, and must never be what makes this path work. A fresh copy of
    // word-stats's already-built library, next to a plugin.ini that
    // declares a 300ms budget instead of word-stats's own (none, so the
    // 4000ms default): "hang" must be killed well under 4000ms.
    QTemporaryDir pluginDir;
    ASSERT_TRUE(pluginDir.isValid());
    ASSERT_TRUE(QFile::copy(wordStatsDir() + QStringLiteral("/libword-stats.so"),
                            pluginDir.filePath(QStringLiteral("libword-stats.so"))));
    QFile ini(pluginDir.filePath(QStringLiteral("plugin.ini")));
    ASSERT_TRUE(ini.open(QIODevice::WriteOnly));
    ini.write(QByteArrayLiteral("[plugin]\nid = word-stats\nlibrary = libword-stats.so\napi = 3\n"
                                "timeout-ms = 1200\n[provides]\ncommands = hang\n"));
    ini.close();

    const PluginManifest manifest = PluginManifest::load(pluginDir.path());
    ASSERT_TRUE(manifest.isValid()) << manifest.error().toStdString();
    ASSERT_EQ(manifest.requestTimeoutMs(), 1200);

    PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacksReturning(QStringLiteral("x")));
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    bool ok = true;
    QString error;
    host.runCommand(QStringLiteral("hang"), [&](bool o, const QJsonObject &, const QString &e) {
        ok = o;
        error = e;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }, 5000));
    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("timed out"))) << error.toStdString();
    // Well under the 4000ms default - proves the manifest's 300ms was the
    // timeout actually armed, not the built-in one.
    EXPECT_LT(clock.elapsed(), 3000);

    host.stop();
}

TEST_F(PluginHostTest, ACrashingCommandDoesNotTakeThisProcessDownWithIt)
{
    PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacksReturning(QStringLiteral("x")));
    bool done = false;
    bool ok = true;
    host.runCommand(QStringLiteral("crash"), [&](bool o, const QJsonObject &, const QString &) {
        ok = o;
        done = true;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_FALSE(ok);
    ASSERT_TRUE(waitUntil([&] { return host.state() == PluginHost::State::Crashed; }));
    EXPECT_GE(host.timing().consecutiveCrashes, 1);
    // Reaching this line at all is the real assertion: abort() happened in
    // a different process, not this test binary.
    SUCCEED();
    host.stop();
}

TEST_F(PluginHostTest, TheSandboxDeniesAFileOutsideThePluginsOwnDirectory)
{
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "bubblewrap not installed on this machine";

    QTemporaryDir homeLike;
    ASSERT_TRUE(homeLike.isValid());
    const QString secretPath = homeLike.filePath(QStringLiteral("secret.txt"));
    QFile secret(secretPath);
    ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
    secret.write("TOP-SECRET");
    secret.close();

    QString notice;
    PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacksReturning(QStringLiteral("x"), &notice));
    bool done = false;
    host.runCommand(QStringLiteral("read-path:") + secretPath,
                    [&](bool, const QJsonObject &, const QString &) { done = true; });
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_EQ(notice, QStringLiteral("cannot-read"));

    host.stop();
}

TEST_F(PluginHostTest, APluginReadsItsOwnConfigAndNoneOfTheAppsOwn)
{
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "bubblewrap not installed on this machine";

    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QString own = configHome.filePath(QStringLiteral("noteAppC/plugins/word-stats/own.ini"));
    const QString apps = configHome.filePath(QStringLiteral("noteAppC/settings.ini"));
    ASSERT_TRUE(writeFile(own, "mine"));
    ASSERT_TRUE(writeFile(apps, "sync.email=me@example.invalid"));

    const auto readAs = [this](const QString &path) {
        QString notice;
        PluginHost host(manifest, {PluginPermission::ReadNotes}, callbacksReturning(QStringLiteral("x"), &notice));
        bool done = false;
        host.runCommand(QStringLiteral("read-path:") + path, [&](bool, const QJsonObject &, const QString &) { done = true; });
        EXPECT_TRUE(waitUntil([&] { return done; }));
        host.stop();
        return notice;
    };
    EXPECT_NE(readAs(own), QStringLiteral("cannot-read"));
    EXPECT_EQ(readAs(apps), QStringLiteral("cannot-read"));
    qputenv("XDG_CONFIG_HOME", previous);
}

// --- PluginRegistry: the community-plugins feature switch -------------------

TEST(PluginRegistryTest, NothingIsReadWhenTheFeatureIsOff)
{
    QTemporaryDir dataHome;
    ASSERT_TRUE(dataHome.isValid());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    const QString pluginDir = PluginRegistry::pluginsRoot() + QStringLiteral("/word-stats");
    ASSERT_TRUE(QDir().mkpath(pluginDir));
    ASSERT_TRUE(
        QFile::copy(wordStatsDir() + QStringLiteral("/plugin.ini"), pluginDir + QStringLiteral("/plugin.ini")));

    PluginRegistry registryOff;
    registryOff.scan(false);
    EXPECT_TRUE(registryOff.installedPlugins().isEmpty());

    PluginRegistry registryOn;
    registryOn.scan(true);
    EXPECT_EQ(registryOn.installedPlugins().size(), 1);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
