// Stage 13 (docs/PLUGINS.md, Tier 1): the Jupyter kernel protocol client.
// Message framing/signing on its own, kernel discovery, the sidecar cache,
// the tier-1 sandbox actually denying a file outside its allowlist, and -
// if a Jupyter kernel is installed - a real end-to-end run against it:
// print(1+1), an execute_result, an error with a traceback, interrupting a
// `while True: pass`, and a restart. GTEST_SKIP when no kernel is found, so
// the minimal environment still passes.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <optional>
#include <QStandardPaths>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "DirectRunner.h"
#include "Ipynb.h"
#include "KernelProcess.h"
#include "KernelSpec.h"
#include "NotebookCache.h"
#include "NotebookController.h"
#include "NotebookMessage.h"
#include "NotebookSandbox.h"
#include "SyscallFilter.h"

#include "WaitFor.h"

#include <algorithm>
#include <unistd.h>
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

TEST(NotebookSandboxTest, WithoutBubblewrapTier1IsRefusedNotRunAsTheUser)
{
    const QByteArray path = qgetenv("PATH");
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    qputenv("PATH", empty.path().toUtf8());
    SandboxRequest request;
    request.runtimeDir = empty.path();
    QString program;
    QStringList args;
    SandboxTier used = SandboxTier::Host;
    wrapCommand(SandboxTier::Bubblewrap, QStringLiteral("/usr/bin/true"), {}, request, &program, &args,
                &used);
    const QString refused = program;
    // Tier 0, which a block has to ask for and Settings has to allow, is
    // unchanged.
    wrapCommand(SandboxTier::Host, QStringLiteral("/usr/bin/true"), {}, request, &program, &args, &used);
    qputenv("PATH", path);
    EXPECT_TRUE(refused.isEmpty());
    EXPECT_EQ(program, QStringLiteral("/usr/bin/true"));
}

TEST(NotebookSandboxTest, Tier1FiltersTheSyscallsThatLeaveANamespace)
{
    if (!detectSandboxAvailability().bubblewrap)
        GTEST_SKIP() << "bubblewrap not installed";

    QTemporaryDir runtimeDir;
    ASSERT_TRUE(runtimeDir.isValid());
    SandboxRequest request;
    request.runtimeDir = runtimeDir.path();
    const auto run = [&](const QStringList &command) {
        QString program;
        QStringList args;
        SandboxTier used;
        int filterFD = -1;
        wrapCommand(SandboxTier::Bubblewrap, command.first(), command.mid(1), request, &program,
                    &args, &used, &filterFD);
        if (filterFD < 0)
            return -2;
        QProcess sandboxed;
        sandboxed.setChildProcessModifier([filterFD] { noteAppC::sandbox::keepInChild(filterFD); });
        sandboxed.start(program, args);
        close(filterFD);
        return sandboxed.waitForFinished(5000) ? sandboxed.exitCode() : -3;
    };

    const int plain = run({QStringLiteral("/usr/bin/true")});
    if (plain == -2)
        GTEST_SKIP() << "built without libseccomp";
    EXPECT_EQ(plain, 0) << "the filter must not stop an ordinary program";
    // Without the filter a nested user namespace works inside bwrap (checked
    // by hand on starbase-1), and with it every capability comes back.
    EXPECT_NE(run({QStringLiteral("/usr/bin/unshare"), QStringLiteral("-U"),
                   QStringLiteral("/usr/bin/true")}),
              0);
    // Threads and fork still work: clone without CLONE_NEWUSER is allowed.
    EXPECT_EQ(run({QStringLiteral("/usr/bin/sh"), QStringLiteral("-c"),
                   QStringLiteral("/usr/bin/true & wait")}),
              0);
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
    // The desktop default tier, so this also proves the sandbox and the
    // kernel work together, not only the sandbox alone (NotebookSandboxTest)
    // or only the protocol alone (a Host-tier run would not catch a bind
    // the kernel actually needs and the sandbox left out).
    request.tier = SandboxTier::Bubblewrap;
    request.useKernel = true; // python would otherwise run without a kernel

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

// --- Jupyter features: shared state, rich output, timeouts ------------------

namespace {

/// Runs `code` through the controller and waits for its answer.
NotebookRunResult runThrough(NotebookController &controller, NotebookController::RunRequest request,
                             const QString &code, int timeoutMs = 20000)
{
    struct Outcome {
        bool delivered = false;
        NotebookRunResult result;
    };
    auto outcome = std::make_shared<Outcome>();
    request.source = code;
    controller.run(request, true, [outcome](NotebookRunResult result) {
        outcome->result = std::move(result);
        outcome->delivered = true;
    });
    EXPECT_TRUE(QTest::qWaitFor([&] { return outcome->delivered; }, timeoutMs))
        << "timed out waiting for a reply to: " << code.toStdString();
    return outcome->result;
}

NotebookController::RunRequest kernelRequest(const QString &scope = QString())
{
    NotebookController::RunRequest request;
    request.language = QStringLiteral("python");
    request.useKernel = true;
    request.tier = SandboxTier::Bubblewrap;
    request.scope = scope;
    return request;
}

/// Isolated runtime and cache directories for a test that starts kernels.
struct KernelTestDirs {
    QTemporaryDir runtimeRoot;
    QTemporaryDir cacheHome;
    KernelTestDirs()
    {
        qputenv("JUPYTER_RUNTIME_DIR", runtimeRoot.path().toUtf8());
        qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    }
};

QString joinedText(const NotebookRunResult &result)
{
    QString text;
    for (const NotebookOutput &output : result.outputs)
        text += output.text;
    return text;
}

} // namespace

TEST(NotebookKernelFeatureTest, ABlocksVariablesLiveOnInItsNotesKernelOnly)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    NotebookController controller;

    const NotebookRunResult first = runThrough(controller, kernelRequest(QStringLiteral("a.md")), QStringLiteral("x = 41"));
    ASSERT_TRUE(first.kernelError.isEmpty()) << first.kernelError.toStdString();
    EXPECT_GT(first.executionCount, 0);

    // The same note's next block sees x, and the count goes up.
    const NotebookRunResult second = runThrough(controller, kernelRequest(QStringLiteral("a.md")), QStringLiteral("print(x + 1)"));
    EXPECT_EQ(joinedText(second).trimmed(), QStringLiteral("42"));
    EXPECT_EQ(second.executionCount, first.executionCount + 1);

    // Another note has its own kernel.
    const NotebookRunResult other = runThrough(controller, kernelRequest(QStringLiteral("b.md")), QStringLiteral("print(x)"));
    EXPECT_FALSE(other.ok);
    EXPECT_EQ(controller.runningKernelCount(QStringLiteral("a.md")), 1);
    EXPECT_EQ(controller.runningKernelCount(QStringLiteral("b.md")), 1);
    EXPECT_EQ(controller.runningKernelCount(), 2);

    // Restarting one note's kernel forgets its variables and leaves the other's alone.
    runThrough(controller, kernelRequest(QStringLiteral("b.md")), QStringLiteral("y = 7"));
    controller.restartKernel(QStringLiteral("python"), QStringLiteral("a.md"));
    EXPECT_EQ(controller.runningKernelCount(QStringLiteral("a.md")), 0);
    EXPECT_FALSE(runThrough(controller, kernelRequest(QStringLiteral("a.md")), QStringLiteral("print(x)")).ok);
    EXPECT_EQ(joinedText(runThrough(controller, kernelRequest(QStringLiteral("b.md")), QStringLiteral("print(y)"))).trimmed(),
              QStringLiteral("7"));

    // The sandbox tier is part of which kernel it is: a host block never sees
    // a sandboxed kernel's variables, nor the other way round.
    NotebookController::RunRequest host = kernelRequest(QStringLiteral("b.md"));
    host.tier = SandboxTier::Host;
    EXPECT_FALSE(runThrough(controller, host, QStringLiteral("print(y)")).ok);
    EXPECT_EQ(controller.runningKernelCount(QStringLiteral("b.md")), 2);
    EXPECT_TRUE(runThrough(controller, host, QStringLiteral("z = 1\nprint(z)")).ok);

    controller.shutdownAll();
}

TEST(NotebookKernelFeatureTest, RichOutputsComeBackInOrderAndInTheirRichestForm)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    NotebookController controller;
    const NotebookController::RunRequest request = kernelRequest();

    // stdout, stderr, stdout: three outputs in the order printed, the middle one a warning.
    NotebookRunResult streams = runThrough(
        controller, request, QStringLiteral("import sys\nprint('out', flush=True)\nprint('err', file=sys.stderr, flush=True)\nprint('again', flush=True)"));
    ASSERT_EQ(streams.outputs.size(), 3);
    EXPECT_FALSE(streams.outputs[0].isStderr);
    EXPECT_TRUE(streams.outputs[1].isStderr);
    EXPECT_EQ(streams.outputs[1].text.trimmed(), QStringLiteral("err"));
    EXPECT_EQ(streams.outputs[2].text.trimmed(), QStringLiteral("again"));

    // A progress bar's carriage returns leave the last state.
    const NotebookRunResult progress = runThrough(controller, request, QStringLiteral("print('10%\\r20%\\r30%')"));
    EXPECT_EQ(joinedText(progress).trimmed(), QStringLiteral("30%"));

    // HTML keeps its markup and its plain twin; LaTeX goes to the maths renderer as $$..$$.
    const NotebookRunResult html = runThrough(
        controller, request, QStringLiteral("from IPython.display import HTML, Latex, display\ndisplay(HTML('<b>hi</b>'))\ndisplay(Latex(r'\\frac{1}{2}'))"));
    ASSERT_EQ(html.outputs.size(), 2);
    EXPECT_EQ(html.outputs[0].kind, NotebookOutput::Kind::Html);
    EXPECT_EQ(html.outputs[0].text, QStringLiteral("<b>hi</b>"));
    EXPECT_FALSE(html.outputs[0].alt.isEmpty());
    EXPECT_EQ(html.outputs[1].kind, NotebookOutput::Kind::Markdown);
    EXPECT_TRUE(html.outputs[1].text.startsWith(QStringLiteral("$$")));
    EXPECT_TRUE(html.outputs[1].text.contains(QStringLiteral("\\frac{1}{2}")));

    // A picture: a 1x1 PNG.
    const NotebookRunResult image = runThrough(
        controller, request,
        QStringLiteral("import base64\nfrom IPython.display import Image, display\n"
                       "display(Image(data=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAHnOcQAAAAABJRU5ErkJggg==')))"));
    ASSERT_EQ(image.outputs.size(), 1);
    EXPECT_EQ(image.outputs[0].kind, NotebookOutput::Kind::ImagePng);
    EXPECT_GT(image.outputs[0].imageBytes.size(), 20);

    // clear_output drops what came before it; update_display_data redraws in place.
    const NotebookRunResult cleared = runThrough(
        controller, request, QStringLiteral("from IPython.display import clear_output\nprint('gone')\nclear_output()\nprint('kept')"));
    EXPECT_EQ(joinedText(cleared).trimmed(), QStringLiteral("kept"));
    const NotebookRunResult updated = runThrough(
        controller, request, QStringLiteral("h = display('one', display_id=True)\nh.update('two')"));
    ASSERT_EQ(updated.outputs.size(), 1);
    EXPECT_TRUE(updated.outputs[0].text.contains(QStringLiteral("two")));

    // What was shown is what the sidecar keeps for the next opening.
    const QString key = notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('kept-in-sidecar')"));
    EXPECT_FALSE(readNotebookCache(key).hasRun);
    runThrough(controller, request, QStringLiteral("print('kept-in-sidecar')"));
    const NotebookRunResult cached = readNotebookCache(key);
    EXPECT_TRUE(cached.hasRun);
    EXPECT_GT(cached.executionCount, 0);
    EXPECT_EQ(joinedText(cached).trimmed(), QStringLiteral("kept-in-sidecar"));
    // `#| cache: false`: run, shown, not kept.
    NotebookController::RunRequest uncached = request;
    uncached.cache = false;
    runThrough(controller, uncached, QStringLiteral("print('not-kept')"));
    EXPECT_FALSE(readNotebookCache(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('not-kept')"))).hasRun);

    controller.shutdownAll();
}

TEST(NotebookKernelFeatureTest, AMatplotlibFigureComesBackAsAPicture)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    QProcess probe;
    probe.start(QStringLiteral("python3"), {QStringLiteral("-c"), QStringLiteral("import matplotlib, matplotlib_inline")});
    if (!probe.waitForFinished(20000) || probe.exitCode() != 0)
        GTEST_SKIP() << "matplotlib is not installed";
    KernelTestDirs dirs;
    NotebookController controller;
    const NotebookRunResult plot = runThrough(
        controller, kernelRequest(),
        QStringLiteral("import matplotlib.pyplot as plt\nplt.plot([1, 2, 3])\nplt.show()"), 60000);
    ASSERT_TRUE(plot.ok) << plot.kernelError.toStdString();
    const bool hasPicture = std::any_of(plot.outputs.cbegin(), plot.outputs.cend(), [](const NotebookOutput &output) {
        return output.kind == NotebookOutput::Kind::ImagePng && output.imageBytes.startsWith("\x89PNG");
    });
    EXPECT_TRUE(hasPicture);
    controller.shutdownAll();
}

TEST(NotebookKernelFeatureTest, ATimedOutRunIsInterruptedAndADeadKernelIsReplaced)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    NotebookController controller;
    NotebookController::RunRequest request = kernelRequest();

    request.timeoutMs = 700;
    const NotebookRunResult slow = runThrough(controller, request, QStringLiteral("import time\ntime.sleep(60)"), 30000);
    EXPECT_FALSE(slow.ok);
    ASSERT_FALSE(slow.outputs.isEmpty());
    EXPECT_EQ(slow.outputs.last().kind, NotebookOutput::Kind::Error);
    EXPECT_EQ(slow.outputs.last().ename, QStringLiteral("Timed out"));
    request.timeoutMs = 0;

    // The kernel survived the interrupt.
    EXPECT_EQ(joinedText(runThrough(controller, request, QStringLiteral("print('alive')"))).trimmed(), QStringLiteral("alive"));

    // Code that kills its own kernel: the block is told, and the next run gets a new one.
    const NotebookRunResult died = runThrough(controller, request, QStringLiteral("import os\nos._exit(3)"), 30000);
    EXPECT_FALSE(died.kernelError.isEmpty());
    EXPECT_EQ(joinedText(runThrough(controller, request, QStringLiteral("print('reborn')"), 30000)).trimmed(),
              QStringLiteral("reborn"));

    // A kernel chosen by its spec's name; one that is not installed says so.
    NotebookController::RunRequest named = request;
    named.useKernel = false;
    named.kernelName = QStringLiteral("python3");
    EXPECT_EQ(joinedText(runThrough(controller, named, QStringLiteral("print('by name')"))).trimmed(), QStringLiteral("by name"));
    named.kernelName = QStringLiteral("no-such-kernel");
    EXPECT_FALSE(runThrough(controller, named, QStringLiteral("1")).kernelError.isEmpty());

    controller.shutdownAll();
}

TEST(NotebookKernelFeatureTest, TheSidecarKeepsTheNewOutputFields)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    NotebookRunResult result;
    result.ok = true;
    result.executionCount = 9;
    NotebookOutput warning;
    warning.text = QStringLiteral("careful");
    warning.isStderr = true;
    NotebookOutput html;
    html.kind = NotebookOutput::Kind::Html;
    html.text = QStringLiteral("<i>x</i>");
    html.alt = QStringLiteral("x");
    result.outputs = {warning, html};
    const QString key = notebookCacheKey(QStringLiteral("python"), QStringLiteral("sidecar"));
    writeNotebookCache(key, result);

    const NotebookRunResult back = readNotebookCache(key);
    ASSERT_EQ(back.outputs.size(), 2);
    EXPECT_EQ(back.executionCount, 9);
    EXPECT_TRUE(back.outputs[0].isStderr);
    EXPECT_EQ(back.outputs[1].kind, NotebookOutput::Kind::Html);
    EXPECT_EQ(back.outputs[1].alt, QStringLiteral("x"));

    removeNotebookCache(key);
    EXPECT_FALSE(readNotebookCache(key).hasRun);
    removeNotebookCache(key); // nothing there is fine
}

// --- the system's own tools, no kernel (DirectRunner) -----------------------

namespace {

/// Runs `source` and waits for the answer; nullopt if none came.
std::optional<NotebookRunResult> runDirect(const QString &language, const QString &source,
                                           SandboxTier tier = SandboxTier::Bubblewrap, int timeoutMs = 30'000,
                                           const QString &wasmModule = QString())
{
    DirectRunner runner;
    std::optional<NotebookRunResult> result;
    DirectRunner::Request request;
    request.language = language;
    request.source = source;
    request.tier = tier;
    request.timeoutMs = timeoutMs;
    request.wasmModule = wasmModule;
    runner.run(request, [&result](NotebookRunResult r) { result = r; });
    (void)QTest::qWaitFor([&] { return result.has_value(); }, 60'000);
    return result;
}

QString textOf(const NotebookRunResult &result)
{
    QString text;
    for (const NotebookOutput &output : result.outputs)
        text += output.kind == NotebookOutput::Kind::Error ? output.traceback.join(QLatin1Char('\n')) : output.text;
    return text;
}

} // namespace

TEST(DirectRunnerTest, HaskellExpressionsPrintTheirValue)
{
    if (!DirectRunner::canRun(QStringLiteral("haskell")))
        GTEST_SKIP() << "ghci is not installed";
    // Jernej's own block, and a definition used by a later line.
    const auto result = runDirect(QStringLiteral("haskell"), QStringLiteral("sum[1..10]\n\nsquare x =\n  x * x\nsquare 7\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("55\n49\n"));
}

TEST(DirectRunnerTest, AHaskellProgramWithMainRuns)
{
    if (!DirectRunner::canRun(QStringLiteral("haskell")) || QStandardPaths::findExecutable(QStringLiteral("runghc")).isEmpty())
        GTEST_SKIP() << "runghc is not installed";
    const auto result = runDirect(QStringLiteral("haskell"), QStringLiteral("main :: IO ()\nmain = print (product [1..5])\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), QStringLiteral("120\n"));
}

TEST(DirectRunnerTest, PythonsLastExpressionPrintsAndErrorsAreErrors)
{
    if (!DirectRunner::canRun(QStringLiteral("python")))
        GTEST_SKIP() << "python3 is not installed";
    auto result = runDirect(QStringLiteral("python"), QStringLiteral("x = 20\nprint('side')\nx + 1\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok);
    EXPECT_EQ(textOf(*result), QStringLiteral("side\n21\n"));

    result = runDirect(QStringLiteral("python"), QStringLiteral("1 / 0\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral("ZeroDivisionError")));
    const auto error = std::find_if(result->outputs.cbegin(), result->outputs.cend(),
                                    [](const NotebookOutput &o) { return o.kind == NotebookOutput::Kind::Error; });
    ASSERT_NE(error, result->outputs.cend());
    EXPECT_EQ(error->ename, QStringLiteral("ZeroDivisionError")); // named as the traceback names it
    EXPECT_EQ(error->evalue, QStringLiteral("division by zero"));

    result = runDirect(QStringLiteral("haskell"), QStringLiteral("undefinedName + 1\n"));
    if (DirectRunner::canRun(QStringLiteral("haskell"))) {
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->ok) << "ghci exits 0 on an error; it is still one";
    }
}

TEST(DirectRunnerTest, ACBlockIsCompiledAndRun)
{
    if (!DirectRunner::canRun(QStringLiteral("c")))
        GTEST_SKIP() << "gcc is not installed";
    const auto result = runDirect(QStringLiteral("c"), QStringLiteral("#include <stdio.h>\nint main(void) { printf(\"%d\\n\", 6 * 7); }\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("42\n"));
}

TEST(DirectRunnerTest, ARunawayBlockIsStoppedAndAnUnknownLanguageSaysSo)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")))
        GTEST_SKIP() << "bash is not installed";
    const auto slow = runDirect(QStringLiteral("bash"), QStringLiteral("sleep 30\n"), SandboxTier::Bubblewrap, 500);
    ASSERT_TRUE(slow.has_value());
    EXPECT_FALSE(slow->ok);
    EXPECT_LT(slow->wallMs, 5000);

    const auto unknown = runDirect(QStringLiteral("brainfuck"), QStringLiteral("+"));
    ASSERT_TRUE(unknown.has_value());
    EXPECT_FALSE(unknown->kernelError.isEmpty());
}

TEST(DirectRunnerTest, TheSandboxKeepsOtherFilesOut)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "bash or bwrap is not installed";
    // A host /tmp file: the sandbox's /tmp is a fresh empty one, and nothing
    // outside the block's own directory is bound in.
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    QFile secret(outside.filePath(QStringLiteral("secret.txt")));
    ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
    secret.write("do not read");
    secret.close();
    const auto result = runDirect(QStringLiteral("bash"), QStringLiteral("cat '%1' || echo refused\n").arg(secret.fileName()));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(textOf(*result).contains(QStringLiteral("do not read")));
    // The host tier, opted into, does read it: the sandbox is what stopped it.
    const auto host = runDirect(QStringLiteral("bash"), QStringLiteral("cat '%1'\n").arg(secret.fileName()), SandboxTier::Host);
    ASSERT_TRUE(host.has_value());
    EXPECT_TRUE(textOf(*host).contains(QStringLiteral("do not read")));
}

// --- the WebAssembly tier ----------------------------------------------------

namespace {

// Prints "hi from wasm" through WASI's fd_write.
constexpr char kHelloWat[] = R"((module
  (import "wasi_snapshot_preview1" "fd_write" (func $w (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 16) "hi from wasm\0a")
  (func (export "_start")
    (i32.store (i32.const 0) (i32.const 16)) (i32.store (i32.const 4) (i32.const 13))
    (drop (call $w (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))))
)";

// An interpreter stand-in: prints its whole argv (NUL separated).
constexpr char kArgvWat[] = R"((module
  (import "wasi_snapshot_preview1" "args_sizes_get" (func $sizes (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "args_get" (func $get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $w (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (func (export "_start")
    (drop (call $sizes (i32.const 0) (i32.const 4)))
    (drop (call $get (i32.const 64) (i32.const 128)))
    (i32.store (i32.const 8) (i32.const 128))
    (i32.store (i32.const 12) (i32.load (i32.const 4)))
    (drop (call $w (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16)))))
)";

bool haveWasmtime()
{
    return !QStandardPaths::findExecutable(QStringLiteral("wasmtime")).isEmpty();
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const char *text)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(text);
    return path;
}

} // namespace

TEST(WasmTierTest, AWatBlockRunsInTheDefaultTier)
{
    if (!haveWasmtime() || !DirectRunner::canRun(QStringLiteral("wat")))
        GTEST_SKIP() << "wasmtime is not installed";
    const auto result = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString() << result->kernelError.toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("hi from wasm\n"));
}

TEST(WasmTierTest, AWatBlockRunsInTheWasmTier)
{
    if (!haveWasmtime())
        GTEST_SKIP() << "wasmtime is not installed";
    const auto result = runDirect(QStringLiteral("wasm"), QString::fromLatin1(kHelloWat), SandboxTier::Wasm);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString() << result->kernelError.toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("hi from wasm\n"));
}

TEST(WasmTierTest, AnotherLanguageRunsThroughItsConfiguredModule)
{
    if (!haveWasmtime())
        GTEST_SKIP() << "wasmtime is not installed";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString module = writeFile(dir, QStringLiteral("interp.wat"), kArgvWat);
    // "python" blocks, though python3 itself is not what runs.
    const auto result = runDirect(QStringLiteral("python"), QStringLiteral("print('x')\n"), SandboxTier::Wasm, 30'000, module);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString() << result->kernelError.toStdString();
    // The module's argv: its own name, then the block's file as the guest sees it.
    const QStringList argv = textOf(*result).split(QChar(0), Qt::SkipEmptyParts);
    ASSERT_EQ(argv.size(), 2) << "python's own -c wrapper must not be passed";
    EXPECT_EQ(argv.last(), QStringLiteral("/cell/cell.py"));
}

TEST(WasmTierTest, WithoutAModuleOrWithoutWasmtimeABlockIsRefused)
{
    if (!haveWasmtime())
        GTEST_SKIP() << "wasmtime is not installed";
    const auto none = runDirect(QStringLiteral("python"), QStringLiteral("print(1)\n"), SandboxTier::Wasm);
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->ok);
    EXPECT_TRUE(none->kernelError.contains(QStringLiteral("no WebAssembly module set for \"python\" blocks")))
        << none->kernelError.toStdString();
    EXPECT_TRUE(none->kernelError.contains(QStringLiteral("notebooks.wasmModules")));

    // wasmtime gone from PATH: refused in the tier, never run another way.
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", "");
    const auto missing = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat), SandboxTier::Wasm);
    qputenv("PATH", path);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->ok);
    EXPECT_TRUE(missing->kernelError.contains(QStringLiteral("wasmtime")));
}

TEST(WasmTierTest, WithoutBubblewrapTheWasmTierRunsWasmtimeAlone)
{
    const QString wasmtime = QStandardPaths::findExecutable(QStringLiteral("wasmtime"));
    if (wasmtime.isEmpty())
        GTEST_SKIP() << "wasmtime is not installed";
    // A PATH holding wasmtime and nothing else, so bwrap is not found.
    QTemporaryDir bin;
    ASSERT_TRUE(bin.isValid());
    ASSERT_TRUE(QFile::link(wasmtime, bin.filePath(QStringLiteral("wasmtime"))));
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()));
    const auto alone = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat), SandboxTier::Wasm);
    // Tier 1 in the same PATH still refuses rather than running unsandboxed.
    const auto refused = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat));
    qputenv("PATH", path);
    ASSERT_TRUE(alone.has_value());
    EXPECT_TRUE(alone->ok) << alone->kernelError.toStdString();
    EXPECT_EQ(textOf(*alone), QStringLiteral("hi from wasm\n"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->ok);
    EXPECT_TRUE(refused->kernelError.contains(QStringLiteral("bubblewrap"))) << refused->kernelError.toStdString();
}

TEST(WasmTierTest, TheModuleSettingIsLookedUpByAnyNameOfTheLanguage)
{
    const QString setting = QStringLiteral(" python = /m/python.wasm ;ruby=/m/ruby.wasm;;junk");
    EXPECT_EQ(DirectRunner::wasmModuleFor(setting, QStringLiteral("python")), QStringLiteral("/m/python.wasm"));
    EXPECT_EQ(DirectRunner::wasmModuleFor(setting, QStringLiteral("PY")), QStringLiteral("/m/python.wasm"));
    EXPECT_EQ(DirectRunner::wasmModuleFor(setting, QStringLiteral("rb")), QStringLiteral("/m/ruby.wasm"));
    EXPECT_TRUE(DirectRunner::wasmModuleFor(setting, QStringLiteral("lua")).isEmpty());
    EXPECT_TRUE(DirectRunner::wasmModuleFor(QString(), QStringLiteral("python")).isEmpty());
}

TEST(WasmTierTest, TheTierIsPartOfTheCacheKey)
{
    const QString source = QStringLiteral("print(1)");
    EXPECT_NE(notebookCacheKey(QStringLiteral("python"), source, SandboxTier::Bubblewrap),
              notebookCacheKey(QStringLiteral("python"), source, SandboxTier::Wasm));
    EXPECT_NE(notebookCacheKey(QStringLiteral("python"), source, SandboxTier::Host),
              notebookCacheKey(QStringLiteral("python"), source, SandboxTier::Wasm));
}

// ASan caught this one: erasing a kernel session killed its process, and
// QProcess::waitForFinished delivered `finished` into a slot that looked the
// session up in the very map that was mid-erase. A process being destroyed
// reports nothing.
TEST(KernelProcessTest, DestroyingARunningProcessEmitsNoFinished)
{
    KernelSpec spec;
    spec.name = QStringLiteral("sleeper");
    spec.argv = {QStringLiteral("sleep"), QStringLiteral("30")};
    auto process = std::make_unique<KernelProcess>();
    if (!process->start(spec, SandboxTier::Host, {}))
        GTEST_SKIP() << process->error().toStdString();
    int finished = 0;
    QObject::connect(process.get(), &KernelProcess::finished, [&finished](int) { ++finished; });
    process.reset();
    EXPECT_EQ(finished, 0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(DirectRunnerTest, ATimeoutOnTheBlockStopsItAndAWarningIsStderr)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    NotebookController controller;
    NotebookController::RunRequest request;
    request.language = QStringLiteral("bash");
    request.tier = SandboxTier::Bubblewrap;

    request.timeoutMs = 400;
    const NotebookRunResult slow = runThrough(controller, request, QStringLiteral("sleep 30"), 30000);
    EXPECT_FALSE(slow.ok);
    ASSERT_FALSE(slow.outputs.isEmpty());
    EXPECT_EQ(slow.outputs.last().ename, QStringLiteral("Timed out"));

    request.timeoutMs = 0;
    const NotebookRunResult warned = runThrough(controller, request, QStringLiteral("echo fine; echo careful >&2"));
    EXPECT_TRUE(warned.ok);
    ASSERT_EQ(warned.outputs.size(), 2);
    EXPECT_FALSE(warned.outputs[0].isStderr);
    EXPECT_TRUE(warned.outputs[1].isStderr);

    // `#| cache: false`: a lookup finds nothing even for a block that ran before.
    const auto lookup = [&](bool cache) {
        NotebookController::RunRequest probe = request;
        probe.source = QStringLiteral("echo fine; echo careful >&2");
        probe.cache = cache;
        std::optional<NotebookRunResult> found;
        controller.run(probe, false, [&](NotebookRunResult result) { found = result; });
        EXPECT_TRUE(QTest::qWaitFor([&] { return found.has_value(); }, 2000));
        return found.value_or(NotebookRunResult());
    };
    EXPECT_TRUE(lookup(true).hasRun);
    EXPECT_FALSE(lookup(false).hasRun);
}

// --- completion and documentation from a running kernel -------------------------

namespace {

/// Asks `controller` for completion or documentation and waits for the answer.
NotebookController::Introspection introspectThrough(NotebookController &controller,
                                                    const NotebookController::RunRequest &request,
                                                    const QString &code, qsizetype cursor, bool inspect,
                                                    bool *sent = nullptr)
{
    struct Outcome {
        bool delivered = false;
        NotebookController::Introspection answer;
    };
    auto outcome = std::make_shared<Outcome>();
    const bool asked = controller.introspect(request, code, cursor, inspect,
                                             [outcome](NotebookController::Introspection answer) {
                                                 outcome->answer = std::move(answer);
                                                 outcome->delivered = true;
                                             });
    if (sent)
        *sent = asked;
    if (asked)
        EXPECT_TRUE(QTest::qWaitFor([&] { return outcome->delivered; }, 20000)) << "no answer for: " << code.toStdString();
    return outcome->answer;
}

} // namespace

TEST(NotebookKernelFeatureTest, CompletionAndDocumentationComeFromTheNotesRunningKernelOnly)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    NotebookController controller;
    const NotebookController::RunRequest request = kernelRequest(QStringLiteral("a.md"));

    // No kernel yet: nothing is asked, and asking does not start one.
    bool sent = true;
    introspectThrough(controller, request, QStringLiteral("pri"), 3, false, &sent);
    EXPECT_FALSE(sent);
    EXPECT_EQ(controller.runningKernelCount(), 0);

    ASSERT_TRUE(runThrough(controller, request, QStringLiteral("import math\nalpha_value = 1\nalpha_other = 2")).ok);

    // What the note's own cells defined is completed...
    const QString code = QStringLiteral("alpha_");
    const auto completion = introspectThrough(controller, request, code, code.size(), false, &sent);
    ASSERT_TRUE(sent);
    ASSERT_TRUE(completion.ok);
    EXPECT_TRUE(completion.matches.contains(QStringLiteral("alpha_value")) || completion.matches.contains(QStringLiteral("alpha_other")));
    EXPECT_EQ(completion.to, code.size());
    EXPECT_LE(completion.from, completion.to);

    // ...and documented. The offsets survive a character outside the BMP.
    const QString emoji = QStringLiteral("s = '\U0001F600'\nmath.sq");
    const auto after = introspectThrough(controller, request, emoji, emoji.size(), false);
    ASSERT_TRUE(after.ok);
    ASSERT_FALSE(after.matches.isEmpty());
    EXPECT_EQ(after.to, emoji.size());
    EXPECT_TRUE(emoji.mid(after.from, after.to - after.from).endsWith(QStringLiteral("sq")));
    const QString docs = QStringLiteral("math.sqrt");
    const auto documented = introspectThrough(controller, request, docs, docs.size(), true);
    EXPECT_TRUE(documented.ok);
    EXPECT_TRUE(documented.text.contains(QStringLiteral("square root"))) << documented.text.toStdString();
    EXPECT_FALSE(documented.text.contains(QLatin1Char('\x1b'))) << "colour codes are stripped";
    const auto unknown = introspectThrough(controller, request, QStringLiteral("nothing_by_this_name"), 20, true);
    EXPECT_TRUE(unknown.text.isEmpty());

    // Another note's kernel is another kernel: not running, so not asked.
    introspectThrough(controller, kernelRequest(QStringLiteral("b.md")), code, code.size(), false, &sent);
    EXPECT_FALSE(sent);
    EXPECT_EQ(controller.runningKernelCount(), 1);

    // A newer question of the same kind makes the older one stale at once.
    bool firstStale = false;
    bool secondAnswered = false;
    ASSERT_TRUE(controller.introspect(request, code, code.size(), false,
                                      [&](NotebookController::Introspection answer) { firstStale = !answer.ok; }));
    ASSERT_TRUE(controller.introspect(request, code, code.size(), false,
                                      [&](NotebookController::Introspection answer) { secondAnswered = answer.ok; }));
    EXPECT_TRUE(firstStale);
    ASSERT_TRUE(QTest::qWaitFor([&] { return secondAnswered; }, 20000));

    // A question still waiting when the kernel goes is answered, empty.
    bool gone = false;
    ASSERT_TRUE(controller.introspect(request, code, code.size(), false,
                                      [&](NotebookController::Introspection answer) { gone = !answer.ok; }));
    controller.restartKernel(QStringLiteral("python"), QStringLiteral("a.md"));
    EXPECT_TRUE(gone);
    controller.shutdownAll();
}

// --- input() from a running block -------------------------------------------

namespace {

/// A missing kernel skips the test, unless the lane check says tools are required.
#define REQUIRE_PYTHON_KERNEL()                                                                          \
    if (!pythonKernelAvailable()) {                                                                      \
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1"))                    \
            << "NOTEAPPC_REQUIRE_TOOLS=1 and no Jupyter python kernelspec is installed";                 \
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";                       \
    }

/// What the stand-in for the person at the prompt saw, and what it answers.
struct Asked {
    QStringList prompts;
    QList<bool> passwords;
    bool cancel = false;
    QString answer;
    int answerDelayMs = 0;
};

NotebookController::RunRequest requestAsking(const std::shared_ptr<Asked> &asked, const QString &scope)
{
    NotebookController::RunRequest request = kernelRequest(scope);
    request.onInput = [asked](const NotebookController::InputPrompt &prompt,
                              const NotebookController::InputReply &reply) {
        asked->prompts << prompt.prompt;
        asked->passwords << prompt.password;
        QTimer::singleShot(asked->answerDelayMs, [asked, reply] { reply(!asked->cancel, asked->answer); });
    };
    return request;
}

} // namespace

TEST(NotebookInputTest, InputAsksAndTheAnswerGoesBackToTheKernel)
{
    REQUIRE_PYTHON_KERNEL();
    KernelTestDirs dirs;
    NotebookController controller;
    auto asked = std::make_shared<Asked>();
    asked->answer = QStringLiteral("Ada \xc5\xa0");
    asked->answerDelayMs = 150; // a person takes a moment: the run is waiting, not finished

    const NotebookRunResult result = runThrough(controller, requestAsking(asked, QStringLiteral("a.md")),
                                                QStringLiteral("name = input('Name: ')\nprint('hi ' + name)"));
    ASSERT_TRUE(result.kernelError.isEmpty()) << result.kernelError.toStdString();
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(asked->prompts, QStringList{QStringLiteral("Name: ")});
    EXPECT_EQ(asked->passwords, QList<bool>{false});
    EXPECT_TRUE(joinedText(result).contains(QStringLiteral("hi Ada \xc5\xa0"))) << joinedText(result).toStdString();

    // A second ask in one cell, and the variable lives on in the kernel.
    asked->prompts.clear();
    asked->answer = QStringLiteral("7");
    const NotebookRunResult twice = runThrough(controller, requestAsking(asked, QStringLiteral("a.md")),
                                               QStringLiteral("a = int(input('a? '))\nb = int(input('b? '))\nprint(a + b)"));
    EXPECT_EQ(asked->prompts, (QStringList{QStringLiteral("a? "), QStringLiteral("b? ")}));
    EXPECT_EQ(joinedText(twice).trimmed(), QStringLiteral("14"));
    controller.shutdownAll();
}

TEST(NotebookInputTest, GetpassIsMaskedAndACancelledInputInterruptsTheRun)
{
    REQUIRE_PYTHON_KERNEL();
    KernelTestDirs dirs;
    NotebookController controller;
    auto asked = std::make_shared<Asked>();
    asked->answer = QStringLiteral("hunter22");

    const NotebookRunResult secret = runThrough(controller, requestAsking(asked, QStringLiteral("a.md")),
                                                QStringLiteral("import getpass\nprint(len(getpass.getpass('Secret: ')))"));
    EXPECT_EQ(asked->passwords, QList<bool>{true});
    EXPECT_EQ(joinedText(secret).trimmed(), QStringLiteral("8"));

    // Esc: the run is interrupted (a KeyboardInterrupt), and the kernel is still there for the next block.
    asked->cancel = true;
    const NotebookRunResult cancelled = runThrough(controller, requestAsking(asked, QStringLiteral("a.md")),
                                                   QStringLiteral("x = input('never answered')\nprint('not reached')"));
    EXPECT_FALSE(cancelled.ok);
    ASSERT_FALSE(cancelled.outputs.isEmpty());
    EXPECT_EQ(cancelled.outputs.last().kind, NotebookOutput::Kind::Error);
    EXPECT_TRUE(cancelled.outputs.last().ename.contains(QStringLiteral("KeyboardInterrupt")))
        << cancelled.outputs.last().ename.toStdString();
    EXPECT_FALSE(joinedText(cancelled).contains(QStringLiteral("not reached")));
    EXPECT_EQ(controller.runningKernelCount(QStringLiteral("a.md")), 1);
    EXPECT_EQ(joinedText(runThrough(controller, kernelRequest(QStringLiteral("a.md")), QStringLiteral("print(1 + 1)"))).trimmed(),
              QStringLiteral("2"));
    controller.shutdownAll();
}

TEST(NotebookInputTest, WithNoOneToAskInputStillRaisesAsItAlwaysDid)
{
    REQUIRE_PYTHON_KERNEL();
    KernelTestDirs dirs;
    NotebookController controller;
    const NotebookRunResult result =
        runThrough(controller, kernelRequest(QStringLiteral("a.md")), QStringLiteral("input('anyone?')"));
    EXPECT_FALSE(result.ok);
    ASSERT_FALSE(result.outputs.isEmpty());
    EXPECT_TRUE(result.outputs.last().ename.contains(QStringLiteral("StdinNotImplementedError")))
        << result.outputs.last().ename.toStdString();
    controller.shutdownAll();
}

TEST(NotebookInputTest, AReplyAfterTheKernelWentIsIgnored)
{
    REQUIRE_PYTHON_KERNEL();
    KernelTestDirs dirs;
    NotebookController controller;
    auto asked = std::make_shared<Asked>();
    NotebookController::InputReply late;
    NotebookController::RunRequest request = kernelRequest(QStringLiteral("a.md"));
    request.onInput = [&](const NotebookController::InputPrompt &, const NotebookController::InputReply &reply) {
        late = reply; // the person is still thinking
    };
    request.source = QStringLiteral("input('wait')");
    bool delivered = false;
    controller.run(request, true, [&](NotebookRunResult) { delivered = true; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return bool(late); }, 20000));
    controller.restartKernel(QStringLiteral("python"), QStringLiteral("a.md")); // delivers a kernel error
    EXPECT_TRUE(delivered);
    late(true, QStringLiteral("too late")); // nothing to send it to: must not crash or wake another kernel
    late(false, QString());
    EXPECT_EQ(controller.runningKernelCount(), 0);
    controller.shutdownAll();
}

// --- .ipynb as a note ---------------------------------------------------------

namespace {

QByteArray sampleNotebook()
{
    QFile file(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/notebook/sample.ipynb"));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

IpynbDocument parsedSample()
{
    IpynbDocument document;
    QString error;
    EXPECT_TRUE(parseIpynb(sampleNotebook(), &document, &error)) << error.toStdString();
    return document;
}

QString joinedSource(const QJsonObject &cell)
{
    QString text;
    for (const QJsonValue &line : cell.value(QStringLiteral("source")).toArray())
        text += line.toString();
    return text;
}

QJsonArray cellsIn(const QByteArray &notebook)
{
    return QJsonDocument::fromJson(notebook).object().value(QStringLiteral("cells")).toArray();
}

/// Does nbformat itself (Jupyter's own validator) accept these bytes? Skipped without Python or the module.
void expectNbformatValid(const QByteArray &bytes)
{
    QProcess python;
    python.start(QStringLiteral("python3"),
                 {QStringLiteral("-c"), QStringLiteral("import sys, nbformat\n"
                                                      "nbformat.validate(nbformat.reads(sys.stdin.read(), 4))")});
    if (!python.waitForStarted()) {
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1")) << "NOTEAPPC_REQUIRE_TOOLS=1 and no python3";
        GTEST_SKIP() << "python3 is not installed";
    }
    python.write(bytes);
    python.closeWriteChannel();
    ASSERT_TRUE(python.waitForFinished(20000));
    const QString err = QString::fromUtf8(python.readAllStandardError());
    if (python.exitCode() != 0 && err.contains(QStringLiteral("No module named 'nbformat'"))) {
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1"))
            << "NOTEAPPC_REQUIRE_TOOLS=1 and python has no nbformat";
        GTEST_SKIP() << "python has no nbformat module";
    }
    EXPECT_EQ(python.exitCode(), 0) << err.toStdString();
}

} // namespace

TEST(IpynbTest, OurJSONWriterIsNbformatsOwn)
{
    // The fixture is nbformat's own output (one-space indent, sorted keys, unicode kept, a final newline):
    // read and written again it is the same bytes, so a saved notebook differs only where it was edited.
    const IpynbDocument document = parsedSample();
    EXPECT_EQ(dumpIpynbJSON(document.root), sampleNotebook());
}

TEST(IpynbTest, CellsBecomeTextAndFences)
{
    const IpynbDocument document = parsedSample();
    EXPECT_EQ(document.language, QStringLiteral("python"));
    const QString text = ipynbMarkdown(document);
    EXPECT_TRUE(text.startsWith(QStringLiteral("# Title\n\nSome *text* with unicode: \u00e9\u00e8 \u2713 and a quote \"x\".\n\n")));
    // Two markdown cells in a row are told apart by a marker line.
    EXPECT_TRUE(text.contains(QStringLiteral("\n\n<!-- cell -->\n\nsecond markdown cell")));
    EXPECT_TRUE(text.contains(QStringLiteral("```python\nimport math\nprint('hello')\nmath.pi\n```\n\n```python\n1/0\n```")));
    // A cell with a fence inside gets a longer one; a raw cell is a raw fence; another language's fence in a
    // markdown cell is just markdown.
    EXPECT_TRUE(text.contains(QStringLiteral("````python\ntext = '''\n```\ninside\n```\n'''\nprint(text)\n````")));
    EXPECT_TRUE(text.contains(QStringLiteral("```raw\nraw text, left alone\n```")));
    EXPECT_TRUE(text.contains(QStringLiteral("```bash\necho not a code cell\n```\n\nafter a bash fence")));
    EXPECT_TRUE(text.endsWith(QLatin1Char('\n')));
}

TEST(IpynbTest, AnUneditedNoteWritesTheFilesOwnBytes)
{
    const QByteArray file = sampleNotebook();
    QString error;
    EXPECT_EQ(mergeIpynb(file, ipynbMarkdown(parsedSample()), &error), file) << error.toStdString();
}

TEST(IpynbTest, EditsKeepEverythingTheTextDoesNotSay)
{
    const QByteArray file = sampleNotebook();
    const QJsonArray before = cellsIn(file);
    QString text = ipynbMarkdown(parsedSample());
    text.replace(QStringLiteral("print('hello')"), QStringLiteral("print('hello, world')"));
    text.replace(QStringLiteral("# Title"), QStringLiteral("# A better title"));

    const QByteArray merged = mergeIpynb(file, text);
    ASSERT_FALSE(merged.isEmpty());
    const QJsonArray after = cellsIn(merged);
    ASSERT_EQ(after.size(), before.size());
    for (int i = 0; i < before.size(); ++i) {
        if (i == 0 || i == 2)
            continue;
        EXPECT_EQ(after.at(i), before.at(i)) << "cell " << i << " was not edited";
    }
    // The edited cells are the same cells with new text: id, metadata, outputs and count stay.
    const QJsonObject code = after.at(2).toObject();
    const QJsonObject codeBefore = before.at(2).toObject();
    EXPECT_EQ(joinedSource(code), QStringLiteral("import math\nprint('hello, world')\nmath.pi"));
    EXPECT_EQ(code.value(QStringLiteral("id")), codeBefore.value(QStringLiteral("id")));
    EXPECT_EQ(code.value(QStringLiteral("metadata")), codeBefore.value(QStringLiteral("metadata")));
    EXPECT_EQ(code.value(QStringLiteral("outputs")), codeBefore.value(QStringLiteral("outputs")));
    EXPECT_EQ(code.value(QStringLiteral("execution_count")), codeBefore.value(QStringLiteral("execution_count")));
    EXPECT_EQ(after.at(0).toObject().value(QStringLiteral("id")), before.at(0).toObject().value(QStringLiteral("id")));
    // Everything outside the cells is as it was, unknown keys included.
    const QJsonObject rootBefore = QJsonDocument::fromJson(file).object();
    QJsonObject rootAfter = QJsonDocument::fromJson(merged).object();
    for (const char *key : {"metadata", "nbformat", "nbformat_minor"})
        EXPECT_EQ(rootAfter.value(QLatin1String(key)), rootBefore.value(QLatin1String(key)));
    EXPECT_TRUE(merged.endsWith("}\n"));
    expectNbformatValid(merged);
}

TEST(IpynbTest, InsertedDeletedAndAdjacentCellsKeepTheirNeighboursAndStayValid)
{
    const QByteArray file = sampleNotebook();
    const QJsonArray before = cellsIn(file);
    QString text = ipynbMarkdown(parsedSample());
    // A new code cell between the two markdown cells and the first code cell; the error cell deleted.
    text.replace(QStringLiteral("\n\n```python\nimport math"), QStringLiteral("\n\n```python\nx = 1\n```\n\n```python\nimport math"));
    text.remove(QStringLiteral("```python\n1/0\n```\n\n"));

    const QByteArray merged = mergeIpynb(file, text);
    const QJsonArray after = cellsIn(merged);
    ASSERT_EQ(after.size(), before.size()); // +1 -1
    EXPECT_EQ(after.at(0), before.at(0));
    EXPECT_EQ(after.at(1), before.at(1)) << "two markdown cells in a row stay two";
    const QJsonObject added = after.at(2).toObject();
    EXPECT_EQ(added.value(QStringLiteral("cell_type")).toString(), QStringLiteral("code"));
    EXPECT_EQ(joinedSource(added), QStringLiteral("x = 1"));
    EXPECT_TRUE(added.value(QStringLiteral("execution_count")).isNull());
    EXPECT_TRUE(added.value(QStringLiteral("outputs")).toArray().isEmpty());
    EXPECT_FALSE(added.value(QStringLiteral("id")).toString().isEmpty()) << "nbformat 4.5 wants an id";
    EXPECT_NE(added.value(QStringLiteral("id")), before.at(2).toObject().value(QStringLiteral("id")));
    EXPECT_EQ(after.at(3), before.at(2)) << "the cell that was there before is untouched, outputs and all";
    EXPECT_EQ(after.at(4), before.at(4)); // the error cell (3) is gone
    EXPECT_EQ(after.at(5), before.at(5));
    EXPECT_EQ(after.at(6), before.at(6));
    expectNbformatValid(merged);
}

TEST(IpynbTest, SomethingThatIsNotANotebookIsRefusedAndNotTouched)
{
    IpynbDocument document;
    QString error;
    EXPECT_FALSE(parseIpynb("not json", &document, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(parseIpynb(R"({"nbformat": 3, "worksheets": []})", &document, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("nbformat 3")));
    EXPECT_FALSE(parseIpynb(R"({"nbformat": 4, "metadata": {}})", &document, &error));
    EXPECT_TRUE(mergeIpynb("not json", QStringLiteral("text"), &error).isEmpty());
    QString text;
    EXPECT_FALSE(readIpynb("[]", &text, &error));
}

TEST(IpynbTest, UnknownKeysSurviveAnEditAndTheLanguageComesFromTheKernelspec)
{
    QJsonObject root = QJsonDocument::fromJson(sampleNotebook()).object();
    root.insert(QStringLiteral("extra_root_key"), QJsonObject{{QStringLiteral("a"), 1}});
    QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("kernelspec"), QJsonObject{{QStringLiteral("language"), QStringLiteral("R")},
                                                              {QStringLiteral("name"), QStringLiteral("ir")}});
    root.insert(QStringLiteral("metadata"), metadata);
    QJsonArray cells = root.value(QStringLiteral("cells")).toArray();
    QJsonObject first = cells.at(0).toObject();
    first.insert(QStringLiteral("future_key"), QStringLiteral("kept"));
    cells.replace(0, first);
    root.insert(QStringLiteral("cells"), cells);
    const QByteArray file = dumpIpynbJSON(root);

    IpynbDocument document;
    ASSERT_TRUE(parseIpynb(file, &document));
    EXPECT_EQ(document.language, QStringLiteral("r"));
    QString text = ipynbMarkdown(document);
    EXPECT_TRUE(text.contains(QStringLiteral("```r\nimport math")));
    text.replace(QStringLiteral("Title"), QStringLiteral("Heading"));
    const QJsonObject merged = QJsonDocument::fromJson(mergeIpynb(file, text)).object();
    EXPECT_EQ(merged.value(QStringLiteral("extra_root_key")), root.value(QStringLiteral("extra_root_key")));
    const QJsonObject edited = merged.value(QStringLiteral("cells")).toArray().at(0).toObject();
    EXPECT_EQ(edited.value(QStringLiteral("future_key")).toString(), QStringLiteral("kept"));
    EXPECT_TRUE(joinedSource(edited).contains(QStringLiteral("Heading")));
}

TEST(IpynbTest, StoredOutputsAreSeededWhereTheEditorLooksForThem)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QString text;
    ASSERT_TRUE(readIpynb(sampleNotebook(), &text));

    const QString key = notebookCacheKey(QStringLiteral("python"), QStringLiteral("import math\nprint('hello')\nmath.pi"));
    const NotebookRunResult first = readNotebookCache(key);
    ASSERT_TRUE(first.hasRun);
    EXPECT_TRUE(first.ok);
    EXPECT_EQ(first.executionCount, 3);
    ASSERT_EQ(first.outputs.size(), 5);
    EXPECT_EQ(first.outputs.at(0).kind, NotebookOutput::Kind::Text);
    EXPECT_EQ(first.outputs.at(0).text, QStringLiteral("hello\n"));
    EXPECT_FALSE(first.outputs.at(0).isStderr);
    EXPECT_TRUE(first.outputs.at(1).isStderr);
    EXPECT_EQ(first.outputs.at(2).text, QStringLiteral("3.141592653589793")); // an execute_result's text/plain
    EXPECT_EQ(first.outputs.at(3).kind, NotebookOutput::Kind::ImagePng);
    EXPECT_TRUE(first.outputs.at(3).imageBytes.startsWith("\x89PNG"));
    EXPECT_EQ(first.outputs.at(4).kind, NotebookOutput::Kind::Html);
    EXPECT_EQ(first.outputs.at(4).text, QStringLiteral("<b>bold</b>"));
    EXPECT_EQ(first.outputs.at(4).alt, QStringLiteral("bold"));

    const NotebookRunResult failed = readNotebookCache(notebookCacheKey(QStringLiteral("python"), QStringLiteral("1/0")));
    ASSERT_TRUE(failed.hasRun);
    EXPECT_FALSE(failed.ok);
    ASSERT_EQ(failed.outputs.size(), 1);
    EXPECT_EQ(failed.outputs.at(0).kind, NotebookOutput::Kind::Error);
    EXPECT_EQ(failed.outputs.at(0).ename, QStringLiteral("ZeroDivisionError"));
    EXPECT_FALSE(failed.outputs.at(0).traceback.join(QLatin1Char('\n')).contains(QLatin1Char('\x1b'))) << "colour codes are stripped";

    // A cell that never ran leaves nothing behind.
    EXPECT_FALSE(readNotebookCache(notebookCacheKey(QStringLiteral("python"), QStringLiteral("text = '''\n```\ninside\n```\n'''\nprint(text)"))).hasRun);

    // And the controller's own lookup - what a note opening does for every block - finds them.
    NotebookController controller;
    NotebookController::RunRequest request;
    request.language = QStringLiteral("python");
    request.source = QStringLiteral("import math\nprint('hello')\nmath.pi");
    bool delivered = false;
    NotebookRunResult looked;
    controller.run(request, false, [&](NotebookRunResult result) {
        looked = std::move(result);
        delivered = true;
    });
    ASSERT_TRUE(QTest::qWaitFor([&] { return delivered; }, 5000));
    EXPECT_TRUE(looked.hasRun);
    EXPECT_EQ(looked.outputs.size(), 5);
}

TEST(IpynbTest, StoredOutputsAreWrittenOnlyWhenLookedUpAndARunWins)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QString text;
    ASSERT_TRUE(readIpynb(sampleNotebook(), &text));
    const QString key = notebookCacheKey(QStringLiteral("python"), QStringLiteral("import math\nprint('hello')\nmath.pi"));
    const QString failedKey = notebookCacheKey(QStringLiteral("python"), QStringLiteral("1/0"));
    EXPECT_TRUE(QDir(notebookCacheRoot()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) << "nothing decoded at read";

    // A run before the first look-up supersedes the file's outputs; a cleared output stays cleared.
    NotebookRunResult ran;
    ran.outputs.append(NotebookOutput{});
    ran.outputs.last().text = QStringLiteral("fresh");
    writeNotebookCache(key, ran);
    removeNotebookCache(failedKey);
    EXPECT_EQ(readNotebookCache(key).outputs.value(0).text, QStringLiteral("fresh"));
    EXPECT_FALSE(readNotebookCache(failedKey).hasRun);

    // Read again, the file's outputs win once more, as they always did at a read.
    ASSERT_TRUE(readIpynb(sampleNotebook(), &text));
    EXPECT_EQ(readNotebookCache(key).outputs.size(), 5);
}
