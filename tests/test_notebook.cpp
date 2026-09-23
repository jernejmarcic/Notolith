// Stage 13 (docs/PLUGINS.md, Tier 1): the Jupyter kernel protocol client.
// Message framing/signing on its own, kernel discovery, the sidecar cache,
// the tier-1 sandbox actually denying a file outside its allowlist, and -
// if a Jupyter kernel is installed - a real end-to-end run against it:
// print(1+1), an execute_result, an error with a traceback, interrupting a
// `while True: pass`, and a restart. GTEST_SKIP when no kernel is found, so
// the minimal environment still passes.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "KernelSpec.h"
#include "NotebookCache.h"
#include "NotebookController.h"
#include "NotebookMessage.h"
#include "NotebookSandbox.h"

#include "WaitFor.h"

#include <algorithm>
#include <memory>

using namespace noteAppC::notebook;

namespace {

bool pythonKernelAvailable()
{
    return kernelSpecsByLanguage().contains(QStringLiteral("python"));
}

} // namespace

// --- message framing and signing -------------------------------------------

TEST(NotebookMessageTest, EncodeDecodeRoundTrip)
{
    const QByteArray key = QByteArrayLiteral("a-test-key");
    QJsonObject content;
    content.insert(QStringLiteral("code"), QStringLiteral("print(1)"));
    const KernelMessage sent =
        buildMessage(newHeader(QStringLiteral("execute_request"), QStringLiteral("session-1"),
                               QStringLiteral("noteAppC")),
                    QJsonObject(), QJsonObject(), content);

    const QList<QByteArray> frames = encodeMessage(sent, key);
    // <IDS|MSG>, signature, header, parent_header, metadata, content - no
    // leading identity frames, since this client is never a ROUTER.
    ASSERT_EQ(frames.size(), 6);
    EXPECT_EQ(frames.at(0), kMessageDelimiter);

    KernelMessage received;
    ASSERT_TRUE(decodeMessage(frames, key, &received));
    EXPECT_EQ(received.msgType(), QStringLiteral("execute_request"));
    EXPECT_EQ(received.content().value(QStringLiteral("code")).toString(), QStringLiteral("print(1)"));
    EXPECT_EQ(received.msgID(), sent.msgID());
}

TEST(NotebookMessageTest, RejectsABadSignature)
{
    const KernelMessage sent = buildMessage(
        newHeader(QStringLiteral("execute_request"), QStringLiteral("s"), QStringLiteral("u")), {}, {},
        QJsonObject());
    QList<QByteArray> frames = encodeMessage(sent, QByteArrayLiteral("real-key"));

    KernelMessage received;
    QString error;
    // Verified with the wrong key: the exact situation a message from
    // anywhere but this app's own kernel would produce.
    EXPECT_FALSE(decodeMessage(frames, QByteArrayLiteral("wrong-key"), &received, &error));
    EXPECT_FALSE(error.isEmpty());

    // Tampering the header after signing must also be caught, not only a
    // mismatched key - the signature covers the bytes, not just "was some
    // key used".
    frames[2] = QByteArrayLiteral(R"({"msg_type":"execute_request","msg_id":"tampered"})");
    EXPECT_FALSE(decodeMessage(frames, QByteArrayLiteral("real-key"), &received, &error));
}

TEST(NotebookMessageTest, DecodeSkipsLeadingIdentityFrames)
{
    // iopub carries a PUB topic frame before the delimiter; a ROUTER-relayed
    // message could carry more than one. Either way, decodeMessage must
    // find the message starting at the delimiter, not at frame 0.
    const KernelMessage sent =
        buildMessage(newHeader(QStringLiteral("status"), QStringLiteral("s"), QStringLiteral("u")), {}, {},
                    QJsonObject{{QStringLiteral("execution_state"), QStringLiteral("busy")}});
    QList<QByteArray> frames = {QByteArrayLiteral("status")}; // the PUB topic
    frames += encodeMessage(sent, QByteArrayLiteral("k"));

    KernelMessage received;
    ASSERT_TRUE(decodeMessage(frames, QByteArrayLiteral("k"), &received));
    EXPECT_EQ(received.msgType(), QStringLiteral("status"));
}

// --- kernel discovery --------------------------------------------------------

TEST(KernelSpecTest, RespectsJupyterPathOverride)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString kernelDir = root.path() + QStringLiteral("/kernels/fake-lang");
    ASSERT_TRUE(QDir().mkpath(kernelDir));
    QFile kernelJSON(kernelDir + QStringLiteral("/kernel.json"));
    ASSERT_TRUE(kernelJSON.open(QIODevice::WriteOnly));
    kernelJSON.write(R"({"argv":["fake","-f","{connection_file}"],)"
                     R"("display_name":"Fake","language":"fakelang"})");
    kernelJSON.close();

    const QByteArray previous = qgetenv("JUPYTER_PATH");
    qputenv("JUPYTER_PATH", root.path().toUtf8());
    const QHash<QString, KernelSpec> specs = kernelSpecsByLanguage();
    if (previous.isNull())
        qunsetenv("JUPYTER_PATH");
    else
        qputenv("JUPYTER_PATH", previous);

    ASSERT_TRUE(specs.contains(QStringLiteral("fakelang")));
    EXPECT_EQ(specs.value(QStringLiteral("fakelang")).name, QStringLiteral("fake-lang"));
    EXPECT_EQ(substituteConnectionFile(specs.value(QStringLiteral("fakelang")).argv, QStringLiteral("/c.json")),
             QStringList({QStringLiteral("fake"), QStringLiteral("-f"), QStringLiteral("/c.json")}));
}

TEST(KernelSpecTest, DiscoversTheInstalledPython3KernelWhenPresent)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    const KernelSpec spec = kernelSpecsByLanguage().value(QStringLiteral("python"));
    EXPECT_EQ(spec.name, QStringLiteral("python3"));
    EXPECT_TRUE(spec.argv.join(QLatin1Char(' ')).contains(QStringLiteral("ipykernel_launcher")));
}

// --- the output sidecar -------------------------------------------------------

TEST(NotebookCacheTest, RoundTripsAndMissesCleanly)
{
    QTemporaryDir cacheHome;
    ASSERT_TRUE(cacheHome.isValid());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());

    const QString key = notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)"));
    EXPECT_FALSE(readNotebookCache(key).hasRun);

    NotebookRunResult result;
    result.ok = true;
    result.wallMs = 42;
    NotebookOutput output;
    output.kind = NotebookOutput::Kind::Text;
    output.text = QStringLiteral("1\n");
    result.outputs.append(output);
    writeNotebookCache(key, result);

    const NotebookRunResult reread = readNotebookCache(key);
    ASSERT_TRUE(reread.hasRun);
    EXPECT_TRUE(reread.ok);
    EXPECT_EQ(reread.wallMs, 42);
    ASSERT_EQ(reread.outputs.size(), 1);
    EXPECT_EQ(reread.outputs.first().text, QStringLiteral("1\n"));

    // Options that only change how the listing looks (title, fold, lines)
    // are never part of the key - only language and source are.
    EXPECT_EQ(key, notebookCacheKey(QStringLiteral("Python"), QStringLiteral("print(1)")));
    EXPECT_NE(key, notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(2)")));
}

// --- the tier-1 sandbox --------------------------------------------------------

TEST(NotebookSandboxTest, Tier1DeniesReadingAFileOutsideTheAllowlist)
{
    if (!detectSandboxAvailability().bubblewrap)
        GTEST_SKIP() << "bubblewrap not installed";

    QTemporaryDir runtimeDir;
    QTemporaryDir secretDir;
    ASSERT_TRUE(runtimeDir.isValid());
    ASSERT_TRUE(secretDir.isValid());
    QFile secret(secretDir.filePath(QStringLiteral("secret.txt")));
    ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
    secret.write("do not read me");
    secret.close();
    const QString secretPath = secret.fileName();

    SandboxRequest request;
    request.runtimeDir = runtimeDir.path();
    QString program;
    QStringList args;
    SandboxTier used;
    wrapCommand(SandboxTier::Bubblewrap, QStringLiteral("/usr/bin/cat"), {secretPath}, request, &program,
               &args, &used);
    ASSERT_EQ(used, SandboxTier::Bubblewrap);

    QProcess sandboxed;
    sandboxed.setStandardOutputFile(QProcess::nullDevice());
    sandboxed.start(program, args);
    ASSERT_TRUE(sandboxed.waitForFinished(5000));
    // Denied: the path simply does not exist inside the sandbox's own
    // mount namespace, so `cat` exits non-zero.
    EXPECT_NE(sandboxed.exitCode(), 0);

    // Positive control, unsandboxed: the same file IS readable - proving
    // the denial above is the sandbox at work, not a typo in the test.
    QProcess direct;
    direct.setStandardOutputFile(QProcess::nullDevice());
    direct.start(QStringLiteral("/usr/bin/cat"), {secretPath});
    ASSERT_TRUE(direct.waitForFinished(5000));
    EXPECT_EQ(direct.exitCode(), 0);
}

TEST(NotebookSandboxTest, Tier1AllowsReadingInsideTheRuntimeDir)
{
    if (!detectSandboxAvailability().bubblewrap)
        GTEST_SKIP() << "bubblewrap not installed";

    QTemporaryDir runtimeDir;
    ASSERT_TRUE(runtimeDir.isValid());
    QFile allowed(runtimeDir.filePath(QStringLiteral("allowed.txt")));
    ASSERT_TRUE(allowed.open(QIODevice::WriteOnly));
    allowed.write("fine");
    allowed.close();

    SandboxRequest request;
    request.runtimeDir = runtimeDir.path();
    QString program;
    QStringList args;
    SandboxTier used;
    wrapCommand(SandboxTier::Bubblewrap, QStringLiteral("/usr/bin/cat"), {allowed.fileName()}, request,
               &program, &args, &used);

    QProcess sandboxed;
    sandboxed.start(program, args);
    ASSERT_TRUE(sandboxed.waitForFinished(5000));
    EXPECT_EQ(sandboxed.exitCode(), 0);
    EXPECT_EQ(sandboxed.readAllStandardOutput(), QByteArrayLiteral("fine"));
}

// --- a real python3 kernel, end to end ----------------------------------------

TEST(NotebookEndToEndTest, RealPython3Kernel)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";

    QTemporaryDir runtimeRoot;
    QTemporaryDir cacheHome;
    ASSERT_TRUE(runtimeRoot.isValid());
    ASSERT_TRUE(cacheHome.isValid());
    qputenv("JUPYTER_RUNTIME_DIR", runtimeRoot.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());

    NotebookController controller;
    NotebookController::RunRequest request;
    request.language = QStringLiteral("python");
    // The desktop default tier, so this also proves the sandbox and the
    // kernel work together, not only the sandbox alone (NotebookSandboxTest)
    // or only the protocol alone (a Host-tier run would not catch a bind
    // the kernel actually needs and the sandbox left out).
    request.tier = SandboxTier::Bubblewrap;

    const auto runAndWait = [&](const QString &code, int timeoutMs = 15000) {
        struct Outcome {
            bool delivered = false;
            NotebookRunResult result;
        };
        auto outcome = std::make_shared<Outcome>();
        NotebookController::RunRequest thisRequest = request;
        thisRequest.source = code;
        controller.run(thisRequest, true, [outcome](NotebookRunResult result) {
            outcome->result = std::move(result);
            outcome->delivered = true;
        });
        // Not WAIT_TRUE: that macro's ASSERT_TRUE returns on failure, which
        // cannot coexist with this lambda's non-void return type. EXPECT_TRUE
        // still fails the test on a timeout, just without the early return.
        EXPECT_TRUE(QTest::qWaitFor([&] { return outcome->delivered; }, timeoutMs))
            << "timed out waiting for a reply to: " << code.toStdString();
        return outcome->result;
    };

    // print(1+1): the answer arrives as iopub "stream" text, not a result.
    const NotebookRunResult printed = runAndWait(QStringLiteral("print(1+1)"));
    ASSERT_TRUE(printed.kernelError.isEmpty()) << printed.kernelError.toStdString();
    EXPECT_TRUE(printed.ok);
    ASSERT_FALSE(printed.outputs.isEmpty());
    EXPECT_EQ(printed.outputs.first().kind, NotebookOutput::Kind::Text);
    EXPECT_EQ(printed.outputs.first().text.trimmed(), QStringLiteral("2"));

    // A bare expression: an execute_result instead.
    const NotebookRunResult result = runAndWait(QStringLiteral("1 + 1"));
    EXPECT_TRUE(result.ok);
    const bool sawResult =
        std::any_of(result.outputs.cbegin(), result.outputs.cend(), [](const NotebookOutput &output) {
            return output.kind == NotebookOutput::Kind::Text && output.text.trimmed() == QStringLiteral("2");
        });
    EXPECT_TRUE(sawResult);

    // An error, with a traceback.
    const NotebookRunResult failed = runAndWait(QStringLiteral("1 / 0"));
    EXPECT_FALSE(failed.ok);
    const auto errorOutput =
        std::find_if(failed.outputs.cbegin(), failed.outputs.cend(), [](const NotebookOutput &output) {
            return output.kind == NotebookOutput::Kind::Error;
        });
    ASSERT_NE(errorOutput, failed.outputs.cend());
    EXPECT_EQ(errorOutput->ename, QStringLiteral("ZeroDivisionError"));
    EXPECT_FALSE(errorOutput->traceback.isEmpty());

    // Interrupting a cell that would otherwise never return.
    struct Outcome {
        bool delivered = false;
        NotebookRunResult result;
    };
    auto interrupted = std::make_shared<Outcome>();
    NotebookController::RunRequest loopRequest = request;
    loopRequest.source = QStringLiteral("while True:\n    pass\n");
    controller.run(loopRequest, true, [interrupted](NotebookRunResult result) {
        interrupted->result = std::move(result);
        interrupted->delivered = true;
    });
    // ponytail: a fixed wait for the kernel to actually enter the loop,
    // not a "busy" signal this test hooks - good enough for a loop that
    // starts executing in well under this, tighten with a real busy-state
    // callback if it ever flakes.
    QTest::qWait(500);
    controller.interruptKernel(QStringLiteral("python"));
    WAIT_TRUE(interrupted->delivered, 15000);
    EXPECT_FALSE(interrupted->result.ok);

    // A restart drops kernel state: `x` from here is gone afterwards.
    runAndWait(QStringLiteral("x = 12345"));
    controller.restartKernel(QStringLiteral("python"));
    const NotebookRunResult afterRestart = runAndWait(QStringLiteral("print(x)"));
    EXPECT_FALSE(afterRestart.ok); // NameError: the new kernel never saw `x`

    controller.shutdownAll();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
