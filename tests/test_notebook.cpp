// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Tier 1 (docs/PLUGINS.md): the Jupyter kernel protocol client.
// Message framing/signing on its own, kernel discovery, the sidecar cache,
// the tier-1 sandbox actually denying a file outside its allowlist, and -
// if a Jupyter kernel is installed - a real end-to-end run against it:
// print(1+1), an execute_result, an error with a traceback, interrupting a
// `while True: pass`, and a restart. GTEST_SKIP when no kernel is found, so
// the minimal environment still passes.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <optional>
#include <QStandardPaths>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "BubblewrapProbe.h"
#include "Bubblewrap.h"
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
#include <sched.h>
#include <sys/wait.h>
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

namespace {

/// `program args` wrapped for tier 1 as the app does it, started with the syscall filter's descriptor handed to the
/// child. False when there is nothing to run (no bubblewrap, no libseccomp).
bool startWrapped(QProcess &process, const SandboxRequest &request, const QString &program, const QStringList &arguments,
                  SandboxTier *used = nullptr)
{
    QString wrapped;
    QStringList args;
    SandboxTier tier = SandboxTier::Bubblewrap;
    int filterFD = -1;
    wrapCommand(SandboxTier::Bubblewrap, program, arguments, request, &wrapped, &args, &tier, &filterFD);
    if (used)
        *used = tier;
    if (wrapped.isEmpty())
        return false;
    process.setChildProcessModifier([filterFD] { noteAppC::sandbox::keepInChild(filterFD); });
    process.start(wrapped, args);
    close(filterFD);
    return true;
}

} // namespace

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
    SandboxTier used;
    QProcess sandboxed;
    sandboxed.setStandardOutputFile(QProcess::nullDevice());
    if (!startWrapped(sandboxed, request, QStringLiteral("/usr/bin/cat"), {secretPath}, &used))
        GTEST_SKIP() << "no bubblewrap with libseccomp";
    ASSERT_EQ(used, SandboxTier::Bubblewrap);
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
    QProcess sandboxed;
    if (!startWrapped(sandboxed, request, QStringLiteral("/usr/bin/cat"), {allowed.fileName()}))
        GTEST_SKIP() << "no bubblewrap with libseccomp";
    ASSERT_TRUE(sandboxed.waitForFinished(5000));
    EXPECT_EQ(sandboxed.exitCode(), 0);
    EXPECT_EQ(sandboxed.readAllStandardOutput(), QByteArrayLiteral("fine"));
}

TEST(NotebookSandboxTest, WithoutBubblewrapNothingRunsAsTheUser)
{
    const QByteArray path = qgetenv("PATH");
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    qputenv("PATH", empty.path().toUtf8());
    SandboxRequest request;
    request.runtimeDir = empty.path();
    // Every tier, the WebAssembly one included, is refused: there is no tier that runs outside the sandbox.
    const auto refused = [&](SandboxTier tier) {
        QString program = QStringLiteral("unset");
        QStringList args{QStringLiteral("unset")};
        SandboxTier used;
        int filterFD = 0;
        wrapCommand(tier, QStringLiteral("/usr/bin/true"), {}, request, &program, &args, &used, &filterFD);
        return program.isEmpty() && args.isEmpty() && filterFD == -1;
    };
    const bool tier1 = refused(SandboxTier::Bubblewrap);
    const bool wasm = refused(SandboxTier::Wasm);
    // The kernel launcher and the diagram/TeX/pandoc command line say no as well.
    KernelSpec spec;
    spec.name = QStringLiteral("sleeper");
    spec.argv = {QStringLiteral("/usr/bin/sleep"), QStringLiteral("30")};
    KernelProcess kernel;
    const bool kernelStarted = kernel.start(spec, SandboxTier::Bubblewrap, {});
    const noteAppC::sandbox::Command diagram = noteAppC::sandbox::bubblewrapCommand(empty.path(), QStringLiteral("/usr/bin/true"), {});
    qputenv("PATH", path);
    EXPECT_TRUE(tier1);
    EXPECT_TRUE(wasm);
    EXPECT_FALSE(kernelStarted);
    EXPECT_TRUE(kernel.error().contains(QStringLiteral("bubblewrap")));
    EXPECT_TRUE(diagram.program.isEmpty());
}

TEST(NotebookSandboxTest, TheCommandLineHasTheNamespacesTheFilterAndNoCapabilities)
{
    if (!detectSandboxAvailability().bubblewrap)
        GTEST_SKIP() << "bubblewrap not installed";
    QTemporaryDir runtimeDir;
    ASSERT_TRUE(runtimeDir.isValid());
    SandboxRequest request;
    request.runtimeDir = runtimeDir.path();
    QString program;
    QStringList args;
    SandboxTier used;
    int filterFD = -1;
    wrapCommand(SandboxTier::Bubblewrap, QStringLiteral("/usr/bin/true"), {}, request, &program, &args, &used, &filterFD);
    if (program.isEmpty())
        GTEST_SKIP() << "built without libseccomp";
    close(filterFD);
    for (const char *flag : {"--unshare-pid", "--unshare-net", "--unshare-user", "--unshare-ipc", "--unshare-uts",
                             "--unshare-cgroup", "--disable-userns", "--die-with-parent", "--new-session", "--clearenv",
                             "--seccomp"})
        EXPECT_TRUE(args.contains(QLatin1String(flag))) << flag;
    EXPECT_FALSE(args.contains(QStringLiteral("--unshare-user-try")));
    EXPECT_FALSE(args.contains(QStringLiteral("--share-net")));
    // Nothing of the home directory or the session is bound, and the process count is limited inside.
    for (const QString &argument : std::as_const(args))
        EXPECT_NE(argument, QDir::homePath());
    EXPECT_TRUE(args.contains(QStringLiteral("--nproc=256")));
    // The filter is part of the options, before the "--" that ends them (bwrap's: a scope's own, in front, is
    // another).
    const qsizetype bwrapAt = args.indexOf(QStringLiteral("--unshare-user"));
    EXPECT_LT(args.indexOf(QStringLiteral("--seccomp")), args.indexOf(QStringLiteral("--"), bwrapAt));
    // /dev/shm is sized like /tmp, after the --dev that made it.
    const qsizetype dev = args.indexOf(QStringLiteral("--dev"));
    ASSERT_GE(dev, 0);
    const QStringList shm = {QStringLiteral("--size"), QStringLiteral("67108864"), QStringLiteral("--tmpfs"), QStringLiteral("/dev/shm")};
    EXPECT_EQ(args.mid(dev + 2, 4), shm);
}

TEST(NotebookSandboxTest, TheSandboxIsPutInACgroupWhenAUserManagerCanTakeIt)
{
    if (!detectSandboxAvailability().bubblewrap)
        GTEST_SKIP() << "bubblewrap not installed";
    QTemporaryDir runtimeDir;
    ASSERT_TRUE(runtimeDir.isValid());
    const auto build = [&](qint64 memory, QString *program, QStringList *args) {
        SandboxRequest request;
        request.runtimeDir = runtimeDir.path();
        request.memoryBytes = memory;
        SandboxTier used;
        int filterFD = -1;
        wrapCommand(SandboxTier::Bubblewrap, QStringLiteral("/usr/bin/true"), {}, request, program, args, &used, &filterFD);
        if (filterFD >= 0)
            close(filterFD);
    };
    QString program;
    QStringList args;
    build(3LL << 30, &program, &args);
    if (program.isEmpty())
        GTEST_SKIP() << "built without libseccomp";
    if (!noteAppC::sandbox::cgroupLimitsAvailable())
        GTEST_SKIP() << "no systemd user manager here";
    EXPECT_TRUE(args.contains(QStringLiteral("--scope")));
    EXPECT_TRUE(args.contains(QStringLiteral("MemoryMax=%1").arg(3LL << 30)));
    EXPECT_TRUE(args.contains(QStringLiteral("MemorySwapMax=0")));
    EXPECT_TRUE(args.contains(QStringLiteral("TasksMax=256")));
    // bwrap is still what is run, after the scope's own "--".
    EXPECT_EQ(args.at(args.indexOf(QStringLiteral("--")) + 1), QStandardPaths::findExecutable(QStringLiteral("bwrap")));

    // Without a user manager the same line is bwrap's own and the rlimits are all there is.
    noteAppC::sandbox::overrideCgroupLimits(false);
    build(3LL << 30, &program, &args);
    noteAppC::sandbox::forgetCgroupProbe();
    EXPECT_EQ(program, QStandardPaths::findExecutable(QStringLiteral("bwrap")));
    EXPECT_FALSE(args.contains(QStringLiteral("--scope")));
    EXPECT_TRUE(args.contains(QStringLiteral("--seccomp")));
}

TEST(NotebookSandboxTest, ACgroupIsOnlyTrustedWhereTheCapIsReallyThere)
{
    if (!noteAppC::sandbox::cgroupLimitsAvailable())
        GTEST_SKIP() << "no systemd user manager with a memory controller here";
    // What the probe vouches for: a scope made by limitInCgroup carries the cap in its own cgroup.
    QString program = QStringLiteral("/bin/sh");
    QStringList args = {QStringLiteral("-c"),
                        QStringLiteral("cat /sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup | head -n 1)/memory.max")};
    noteAppC::sandbox::limitInCgroup(program, args, 3LL << 30);
    ASSERT_NE(program, QStringLiteral("/bin/sh"));
    QProcess process;
    process.start(program, args);
    ASSERT_TRUE(process.waitForFinished(10000));
    EXPECT_EQ(process.readAllStandardOutput().trimmed(), QByteArray::number(3LL << 30));
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
    // by hand on a desktop), and with it every capability comes back.
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
    // Limit: a fixed wait for the kernel to actually enter the loop,
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

    controller.shutdownAll();
}

TEST(NotebookKernelFeatureTest, ThePreludeRunsOnceAndAgainAfterARestart)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    NotebookController controller;
    NotebookController::RunRequest request = kernelRequest(QStringLiteral("p.md"));
    request.prelude = QStringLiteral("import math\ncounter = globals().get('counter', 0) + 1");

    const NotebookRunResult a = runThrough(controller, request, QStringLiteral("print(counter)"));
    ASSERT_TRUE(a.kernelError.isEmpty()) << a.kernelError.toStdString();
    EXPECT_EQ(joinedText(a).trimmed(), QStringLiteral("1"));
    EXPECT_EQ(a.executionCount, 1); // the prelude is not a cell of its own
    const NotebookRunResult b = runThrough(controller, request, QStringLiteral("print(counter, math.pi > 3)"));
    EXPECT_EQ(joinedText(b).trimmed(), QStringLiteral("1 True")); // sent once, not with every block
    EXPECT_EQ(b.executionCount, 2);

    controller.restartKernel(QStringLiteral("python"), QStringLiteral("p.md"));
    const NotebookRunResult c = runThrough(controller, request, QStringLiteral("print(counter)"));
    EXPECT_EQ(joinedText(c).trimmed(), QStringLiteral("1")); // a new kernel gets it again

    // A wrapper with nowhere to put the block is ignored, not run instead of it.
    NotebookController::RunRequest noSlot = kernelRequest(QStringLiteral("r.md"));
    noSlot.wrapper = QStringLiteral("x = 1");
    EXPECT_EQ(joinedText(runThrough(controller, noSlot, QStringLiteral("print('ran')"))).trimmed(), QStringLiteral("ran"));

    // A prelude that fails is said so on the block's result, not swallowed.
    NotebookController::RunRequest broken = kernelRequest(QStringLiteral("q.md"));
    broken.prelude = QStringLiteral("import nosuchmodule_q");
    const NotebookRunResult d = runThrough(controller, broken, QStringLiteral("print('still runs')"));
    EXPECT_FALSE(d.ok);
    ASSERT_FALSE(d.outputs.isEmpty());
    EXPECT_EQ(d.outputs.first().kind, NotebookOutput::Kind::Error);
    EXPECT_TRUE(d.outputs.first().ename.startsWith(QStringLiteral("implicit code:"))) << d.outputs.first().ename.toStdString();
    EXPECT_TRUE(joinedText(d).contains(QStringLiteral("still runs")));
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

TEST(NotebookKernelFeatureTest, ATimeoutCountsFromTheRunNotTheKernelsStartUp)
{
    if (!pythonKernelAvailable())
        GTEST_SKIP() << "no Jupyter python kernelspec installed on this machine";
    KernelTestDirs dirs;
    // A kernel that takes longer to start than the block's timeout, as a
    // cold one does on a loaded machine.
    QTemporaryDir root;
    const QString kernelDir = root.path() + QStringLiteral("/kernels/slow-python");
    ASSERT_TRUE(QDir().mkpath(kernelDir));
    QFile kernelJSON(kernelDir + QStringLiteral("/kernel.json"));
    ASSERT_TRUE(kernelJSON.open(QIODevice::WriteOnly));
    kernelJSON.write(R"({"argv":["sh","-c","sleep 1.5; exec python -m ipykernel_launcher -f \"$0\"","{connection_file}"],)"
                     R"("display_name":"Slow","language":"python"})");
    kernelJSON.close();
    const QByteArray previous = qgetenv("JUPYTER_PATH");
    qputenv("JUPYTER_PATH", root.path().toUtf8());
    const auto restore = qScopeGuard([&] {
        if (previous.isNull())
            qunsetenv("JUPYTER_PATH");
        else
            qputenv("JUPYTER_PATH", previous);
    });

    NotebookController controller;
    NotebookController::RunRequest request = kernelRequest();
    request.kernelName = QStringLiteral("slow-python");
    request.timeoutMs = 700;
    const NotebookRunResult slow = runThrough(controller, request, QStringLiteral("import time\ntime.sleep(60)"), 30000);
    EXPECT_TRUE(slow.kernelError.isEmpty()) << slow.kernelError.toStdString() << " - interrupted while starting";
    ASSERT_FALSE(slow.outputs.isEmpty());
    EXPECT_EQ(slow.outputs.last().ename, QStringLiteral("Timed out"));
    request.timeoutMs = 0;
    EXPECT_EQ(joinedText(runThrough(controller, request, QStringLiteral("print('alive')"))).trimmed(), QStringLiteral("alive"));
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
                                           const QString &wasmModule = QString(),
                                           const QStringList &readOnlyBinds = {})
{
    DirectRunner runner;
    std::optional<NotebookRunResult> result;
    DirectRunner::Request request;
    request.language = language;
    request.source = source;
    request.tier = tier;
    request.timeoutMs = timeoutMs;
    request.wasmModule = wasmModule;
    request.readOnlyBinds = readOnlyBinds;
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
    // A block as a user would write it, and a definition used by a later line.
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

namespace {

/// A snippet with no `main` is checked, not run: ok with the one line, or
/// the compiler's own error. `tool` is the extra program a language needs.
void expectCheckedWhenNoMain(const QString &language, const QString &good, const QString &bad, const QString &error,
                             const QString &tool)
{
    if (!DirectRunner::canRun(language) || QStandardPaths::findExecutable(tool).isEmpty())
        GTEST_SKIP() << tool.toStdString() << " is not installed";
    auto result = runDirect(language, good);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral("Nothing to run"))) << textOf(*result).toStdString();

    result = runDirect(language, bad);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_TRUE(textOf(*result).contains(error)) << textOf(*result).toStdString();
}

} // namespace

TEST(DirectRunnerTest, ARustBlockWithNoMainIsCheckedNotRun)
{
    expectCheckedWhenNoMain(QStringLiteral("rust"),
                            QStringLiteral("struct Note { title: String }\nimpl Note { fn slug(&self) -> String { self.title.to_lowercase() } }\n"),
                            QStringLiteral("fn f() -> u32 { \"x\" }\n"), QStringLiteral("E0308"), QStringLiteral("rustc"));
}

TEST(DirectRunnerTest, ACBlockWithNoMainIsCheckedNotRun)
{
    expectCheckedWhenNoMain(QStringLiteral("c"), QStringLiteral("static int twice(int x) { return 2 * x; }\n"),
                            QStringLiteral("int f(void) { return undeclared; }\n"), QStringLiteral("undeclared"),
                            QStringLiteral("gcc"));
}

TEST(DirectRunnerTest, ACppBlockWithNoMainIsCheckedNotRun)
{
    expectCheckedWhenNoMain(QStringLiteral("cpp"), QStringLiteral("struct P { int x; int twice() const { return 2 * x; } };\n"),
                            QStringLiteral("int f() { return undeclared; }\n"), QStringLiteral("undeclared"),
                            QStringLiteral("g++"));
}

TEST(DirectRunnerTest, AJavaBlockWithNoMainIsCheckedNotRun)
{
    expectCheckedWhenNoMain(QStringLiteral("java"), QStringLiteral("class Note { int twice(int x) { return 2 * x; } }\n"),
                            QStringLiteral("class Note { int f() { return undeclared; } }\n"), QStringLiteral("undeclared"),
                            QStringLiteral("javac"));
}

TEST(DirectRunnerTest, BlocksWithMainStillRunAndAMainInACommentDoesNotCount)
{
    if (!DirectRunner::canRun(QStringLiteral("c")) || !DirectRunner::canRun(QStringLiteral("rust")))
        GTEST_SKIP() << "gcc or rustc is not installed";
    auto result = runDirect(QStringLiteral("rust"), QStringLiteral("fn main() { println!(\"{}\", 6 * 7); }\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(textOf(*result), QStringLiteral("42\n"));
    result = runDirect(QStringLiteral("c"), QStringLiteral("/* main( */ // int main(\nint f(void) { return 1; }\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral("Nothing to run"))) << textOf(*result).toStdString();
}

// --- implicit code: a note's frontmatter prelude and wrapper ---------------

namespace {

/// Runs `source` with the note's implicit code and waits for the answer.
std::optional<NotebookRunResult> runImplicit(const QString &language, const QString &source, const QString &prelude,
                                             const QString &wrapper = QString())
{
    DirectRunner runner;
    std::optional<NotebookRunResult> result;
    DirectRunner::Request request;
    request.language = language;
    request.source = source;
    request.prelude = prelude;
    request.wrapper = wrapper;
    runner.run(request, [&result](NotebookRunResult r) { result = r; });
    (void)QTest::qWaitFor([&] { return result.has_value(); }, 60'000);
    return result;
}

} // namespace

TEST(DirectRunnerTest, ImplicitCodeIsPrependedForACompiledLanguage)
{
    if (!DirectRunner::canRun(QStringLiteral("rust")) || !DirectRunner::canRun(QStringLiteral("python")))
        GTEST_SKIP() << "rustc or python3 is not installed";
    auto result = runImplicit(QStringLiteral("rust"),
                              QStringLiteral("fn main() { let m: HashMap<u8, u8> = HashMap::new(); println!(\"{}\", m.len()); }\n"),
                              QStringLiteral("use std::collections::HashMap;"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("0\n"));
    result = runImplicit(QStringLiteral("python"), QStringLiteral("print(math.pi > 3)"), QStringLiteral("import math"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("True\n"));
}

TEST(DirectRunnerTest, AnErrorLineIsTheBlocksOwn)
{
    if (!DirectRunner::canRun(QStringLiteral("python")))
        GTEST_SKIP() << "python3 is not installed";
    auto result = runImplicit(QStringLiteral("python"), QStringLiteral("x = 1\n1/0"), QStringLiteral("import math\nimport os"));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    // Line 2 of the block, not line 4 of the file that ran.
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral(", line 2,"))) << textOf(*result).toStdString();
    EXPECT_FALSE(textOf(*result).contains(QStringLiteral(", line 4,"))) << textOf(*result).toStdString();

    result = runImplicit(QStringLiteral("python"), QStringLiteral("pass"), QStringLiteral("import nosuchmodule_q"));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral("implicit code, line 1"))) << textOf(*result).toStdString();
}

TEST(DirectRunnerTest, AWrapperRunsABlockWithNoMain)
{
    if (!DirectRunner::canRun(QStringLiteral("rust")))
        GTEST_SKIP() << "rustc is not installed";
    const QString wrapper = QStringLiteral("fn main() {\n{{code}}\n}");
    auto result = runImplicit(QStringLiteral("rust"), QStringLiteral("println!(\"hi\");"), QString(), wrapper);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("hi\n"));
    EXPECT_FALSE(textOf(*result).contains(QStringLiteral("Checked")));

    // A block with a main of its own is not wrapped a second time.
    result = runImplicit(QStringLiteral("rust"), QStringLiteral("fn main() { println!(\"once\"); }"), QString(), wrapper);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("once\n"));

    // And a wrapper's own lines are not the block's: line 1 of this block is line 2 of the file.
    result = runImplicit(QStringLiteral("rust"), QStringLiteral("let n: u8 = \"x\";"), QString(), wrapper);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_TRUE(textOf(*result).contains(QStringLiteral("cell.rs:1:"))) << textOf(*result).toStdString();
}

TEST(DirectRunnerTest, ImportsOnlyAndNoMainIsStillCheckOnly)
{
    if (!DirectRunner::canRun(QStringLiteral("c")))
        GTEST_SKIP() << "gcc is not installed";
    // size_t is only declared by the prelude: the check sees it, and still finds no main.
    const auto result = runImplicit(QStringLiteral("c"), QStringLiteral("size_t twice(size_t x) { return 2 * x; }"),
                                    QStringLiteral("#include <stddef.h>"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << textOf(*result).toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("Checked, no errors. Nothing to run: this block has no main.\n"));
}

TEST(DirectRunnerTest, WithoutImplicitCodeTheRunnerInputIsByteIdentical)
{
    DirectRunner::Request request;
    request.language = QStringLiteral("rust");
    int offset = -1;
    for (const QString &source : {QStringLiteral("fn main() {}"), QStringLiteral("fn main() {}\n"), QString()}) {
        request.source = source;
        EXPECT_EQ(DirectRunner::composed(request, &offset), source);
        EXPECT_EQ(offset, 0);
    }
    request.source = QStringLiteral("let x = 1;");
    request.prelude = QStringLiteral("use a;\nuse b;");
    request.wrapper = QStringLiteral("fn main() {\n{{code}}\n}");
    EXPECT_EQ(DirectRunner::composed(request, &offset), QStringLiteral("use a;\nuse b;\nfn main() {\nlet x = 1;\n}"));
    EXPECT_EQ(offset, 3);

    // The cache key: unchanged without implicit code, different with it.
    const QString plain = notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('p5')"), SandboxTier::Bubblewrap);
    // Spelled out, so that a note without implicit code keeps the keys (and the sidecar entries) it already has.
    EXPECT_EQ(plain, QString::fromLatin1(QCryptographicHash::hash(QByteArray("python\x1fprint('p5')\x1f") + QByteArray::number(int(SandboxTier::Bubblewrap)),
                                                                  QCryptographicHash::Sha256).toHex()));
    EXPECT_NE(plain, notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('p5')"), SandboxTier::Bubblewrap,
                                      QStringLiteral("import os")));
    EXPECT_NE(notebookCacheKey(QStringLiteral("python"), QStringLiteral("x"), SandboxTier::Bubblewrap, QStringLiteral("a")),
              notebookCacheKey(QStringLiteral("python"), QStringLiteral("x"), SandboxTier::Bubblewrap, QString(), QStringLiteral("a")));

    if (!DirectRunner::canRun(QStringLiteral("python")))
        GTEST_SKIP() << "python3 is not installed";
    KernelTestDirs dirs;
    NotebookController controller;
    NotebookController::RunRequest run;
    run.language = QStringLiteral("python");
    ASSERT_TRUE(runThrough(controller, run, QStringLiteral("print('p5')")).ok);
    EXPECT_TRUE(readNotebookCache(plain).hasRun);
    run.prelude = QStringLiteral("import os");
    ASSERT_TRUE(runThrough(controller, run, QStringLiteral("print('p5b')")).ok);
    EXPECT_FALSE(readNotebookCache(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('p5b')"))).hasRun);
    EXPECT_TRUE(readNotebookCache(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print('p5b')"),
                                                   SandboxTier::Bubblewrap, QStringLiteral("import os")))
                    .hasRun);
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
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !bubblewrapUsable())
        GTEST_SKIP() << "bash is not installed, or bubblewrap is missing or cannot make a user namespace here";
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
    // The same read, unsandboxed, works: the sandbox is what stopped it.
    QFile direct(secret.fileName());
    ASSERT_TRUE(direct.open(QIODevice::ReadOnly));
    EXPECT_EQ(direct.readAll(), QByteArrayLiteral("do not read"));
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

// Prints the name of the guest's first preopened directory (fd 3), then
// " and more" if there is a second one (fd 4).
constexpr char kPreopensWat[] = R"((module
  (import "wasi_snapshot_preview1" "fd_prestat_get" (func $p (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_prestat_dir_name" (func $n (param i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $w (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 200) "\0a")
  (data (i32.const 210) " and more\0a")
  (func (export "_start")
    (drop (call $p (i32.const 3) (i32.const 32)))
    (drop (call $n (i32.const 3) (i32.const 64) (i32.load (i32.const 36))))
    (i32.store (i32.const 0) (i32.const 64))
    (i32.store (i32.const 4) (i32.load (i32.const 36)))
    (i32.store (i32.const 8) (select (i32.const 210) (i32.const 200) (i32.eqz (call $p (i32.const 4) (i32.const 40)))))
    (i32.store (i32.const 12) (select (i32.const 10) (i32.const 1) (i32.eqz (call $p (i32.const 4) (i32.const 40)))))
    (drop (call $w (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 16)))
    (drop (call $w (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 16))))))";

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

TEST(WasmTierTest, WithoutBubblewrapTheWasmTierRefusesAsTierOneDoes)
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
    // Never wasmtime alone: no seccomp, no network namespace.
    const auto wasm = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat), SandboxTier::Wasm);
    const auto tierOne = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat));
    qputenv("PATH", path);
    for (const auto &refused : {wasm, tierOne}) {
        ASSERT_TRUE(refused.has_value());
        EXPECT_FALSE(refused->ok);
        EXPECT_TRUE(refused->kernelError.contains(QStringLiteral("bubblewrap"))) << refused->kernelError.toStdString();
    }
}

TEST(WasmTierTest, TheGuestSeesOnlyItsOwnDirectory)
{
    if (!haveWasmtime())
        GTEST_SKIP() << "wasmtime is not installed";
    // A runnable embed's folder is asked for and must still not be granted.
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    const auto result = runDirect(QStringLiteral("wat"), QString::fromLatin1(kPreopensWat), SandboxTier::Wasm, 30'000,
                                  QString(), {vault.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << result->kernelError.toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("/cell\n"));
}

TEST(WasmTierTest, ASymlinkedWasmtimeRunsInsideBubblewrap)
{
    const QString wasmtime = QStandardPaths::findExecutable(QStringLiteral("wasmtime"));
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (wasmtime.isEmpty() || bwrap.isEmpty())
        GTEST_SKIP() << "needs wasmtime and bubblewrap";
    // wasmtime found through a link outside /usr (a shim, Homebrew, Nix): the
    // link itself does not exist inside the sandbox, its target does.
    QTemporaryDir bin;
    ASSERT_TRUE(bin.isValid());
    ASSERT_TRUE(QFile::link(wasmtime, bin.filePath(QStringLiteral("wasmtime"))));
    ASSERT_TRUE(QFile::link(bwrap, bin.filePath(QStringLiteral("bwrap"))));
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()));
    const auto result = runDirect(QStringLiteral("wat"), QString::fromLatin1(kHelloWat), SandboxTier::Wasm);
    qputenv("PATH", path);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << result->kernelError.toStdString();
    EXPECT_EQ(textOf(*result), QStringLiteral("hi from wasm\n"));
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
    if (!process->start(spec, SandboxTier::Bubblewrap, {}))
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

/// A missing kernel skips the test, unless the check script says tools are required.
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

TEST(IpynbTest, ACellsOwnSandboxLineChoosesTheKeyItsOutputIsSeededUnder)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const auto cell = [](const char *source) {
        return QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("code")},
                           {QStringLiteral("source"), QString::fromLatin1(source)},
                           {QStringLiteral("metadata"), QJsonObject()},
                           {QStringLiteral("execution_count"), 1},
                           {QStringLiteral("outputs"), QJsonArray{QJsonObject{{QStringLiteral("output_type"), QStringLiteral("stream")},
                                                                              {QStringLiteral("name"), QStringLiteral("stdout")},
                                                                              {QStringLiteral("text"), QStringLiteral("1\n")}}}}};
    };
    const QJsonObject root{{QStringLiteral("nbformat"), 4}, {QStringLiteral("nbformat_minor"), 5},
                           {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("language_info"),
                                                                     QJsonObject{{QStringLiteral("name"), QStringLiteral("python")}}}}},
                           {QStringLiteral("cells"), QJsonArray{cell("#| sandbox: wasm\nprint(1)"), cell("#| sandbox: host\nprint(2)")}}};
    QString text;
    ASSERT_TRUE(readIpynb(QJsonDocument(root).toJson(), &text));
    const auto seeded = [](const QString &source, SandboxTier tier) {
        return readNotebookCache(notebookCacheKey(QStringLiteral("python"), source, tier)).hasRun;
    };
    const QString wasm = QStringLiteral("#| sandbox: wasm\nprint(1)");
    EXPECT_TRUE(seeded(wasm, SandboxTier::Wasm));
    EXPECT_FALSE(seeded(wasm, SandboxTier::Bubblewrap));
    // There is no host tier: that cell runs in tier 1 and is found under its key.
    const QString host = QStringLiteral("#| sandbox: host\nprint(2)");
    EXPECT_TRUE(seeded(host, SandboxTier::Bubblewrap));
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

// --- writing a run back into the .ipynb ----------------------------------------

namespace {

QJsonObject streamOutput(const QString &text)
{
    return {{QStringLiteral("output_type"), QStringLiteral("stream")},
            {QStringLiteral("name"), QStringLiteral("stdout")},
            {QStringLiteral("text"), QJsonArray{text}}};
}

QJsonObject codeCell(const QString &id, const QString &source, int count, const QString &output)
{
    return {{QStringLiteral("cell_type"), QStringLiteral("code")},
            {QStringLiteral("id"), id},
            {QStringLiteral("metadata"), QJsonObject()},
            {QStringLiteral("source"), QJsonArray{source}},
            {QStringLiteral("execution_count"), count > 0 ? QJsonValue(count) : QJsonValue()},
            {QStringLiteral("outputs"), output.isEmpty() ? QJsonArray() : QJsonArray{streamOutput(output)}}};
}

QByteArray notebookOf(const QList<QJsonObject> &cells)
{
    QJsonArray array;
    for (const QJsonObject &cell : cells)
        array.append(cell);
    return dumpIpynbJSON(
        {{QStringLiteral("nbformat"), 4},
         {QStringLiteral("nbformat_minor"), 5},
         {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("language_info"),
                                                   QJsonObject{{QStringLiteral("name"), QStringLiteral("python")}}}}},
         {QStringLiteral("cells"), array}});
}

QString textOf(const QByteArray &file)
{
    IpynbDocument document;
    EXPECT_TRUE(parseIpynb(file, &document));
    return ipynbMarkdown(document);
}

QString pyKey(const QString &source, int ordinal = 0)
{
    return notebookCacheKey(QStringLiteral("python"), source, SandboxTier::Bubblewrap, {}, {}, ordinal);
}

NotebookRunResult printed(const QString &text, int count)
{
    NotebookRunResult result;
    result.executionCount = count;
    NotebookOutput output;
    output.text = text;
    result.outputs.append(output);
    return result;
}

/// What the window holds once these keys' cells ran: whatever the cache holds under each.
IpynbRuns runsOf(std::initializer_list<QString> keys)
{
    IpynbRuns runs;
    for (const QString &key : keys)
        runs.results.insert(key, readNotebookCache(key));
    return runs;
}

QJsonObject cellAt(const QByteArray &file, int index)
{
    return cellsIn(file).at(index).toObject();
}

QByteArray dumped(const QJsonObject &cell)
{
    return dumpIpynbJSON(cell);
}

} // namespace

TEST(NotebookCacheKeyTest, ADuplicateOrdinalOfZeroKeepsEveryExistingKey)
{
    const QString legacy = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray("python\x1f" "print(1)\x1f") + QByteArray::number(int(SandboxTier::Bubblewrap)),
                                 QCryptographicHash::Sha256).toHex());
    EXPECT_EQ(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)")), legacy);
    EXPECT_EQ(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)"), SandboxTier::Bubblewrap, {}, {}, 0), legacy);
    EXPECT_NE(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)"), SandboxTier::Bubblewrap, {}, {}, 1), legacy);
    EXPECT_NE(notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)"), SandboxTier::Bubblewrap, {}, {}, 1),
              notebookCacheKey(QStringLiteral("python"), QStringLiteral("print(1)"), SandboxTier::Bubblewrap, {}, {}, 2));
}

TEST(NotebookCacheKeyTest, TheOrdinalCountsEarlierBlocksThatReadTheSame)
{
    const QStringList sources = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("a"),
                                 QStringLiteral("b")};
    EXPECT_EQ(duplicateOrdinal(sources, 0), 0);
    EXPECT_EQ(duplicateOrdinal(sources, 1), 0);
    EXPECT_EQ(duplicateOrdinal(sources, 2), 1);
    EXPECT_EQ(duplicateOrdinal(sources, 3), 2);
    EXPECT_EQ(duplicateOrdinal(sources, 4), 1);
    EXPECT_EQ(duplicateOrdinals(sources), (QList<int>{0, 0, 1, 2, 1}));
}

TEST(IpynbWriteBackTest, DuplicateCellsGetTheirOwnKeysAndTheirOwnSeededOutput)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("a"), QStringLiteral("print('x')"), 1, QStringLiteral("first\n")),
                                        codeCell(QStringLiteral("b"), QStringLiteral("print('x')"), 2, QStringLiteral("second\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    EXPECT_EQ(readNotebookCache(pyKey(QStringLiteral("print('x')"), 0)).outputs.value(0).text, QStringLiteral("first\n"));
    const NotebookRunResult second = readNotebookCache(pyKey(QStringLiteral("print('x')"), 1));
    ASSERT_TRUE(second.hasRun);
    EXPECT_EQ(second.outputs.value(0).text, QStringLiteral("second\n"));
    EXPECT_EQ(second.executionCount, 2);
}

TEST(IpynbWriteBackTest, ARunOfCellTwoOfThreeIsSavedOnCellTwoAlone)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("c1"), QStringLiteral("print(1)"), 1, QStringLiteral("1\n")),
                                        codeCell(QStringLiteral("c2"), QStringLiteral("print(2)"), 0, QString()),
                                        codeCell(QStringLiteral("c3"), QStringLiteral("print(3)"), 3, QStringLiteral("3\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));

    writeNotebookCache(pyKey(QStringLiteral("print(2)")), printed(QStringLiteral("2\n"), 7));
    const QByteArray merged = mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("print(2)"))}));
    ASSERT_FALSE(merged.isEmpty());
    EXPECT_EQ(dumped(cellAt(merged, 0)), dumped(cellAt(file, 0))) << "an untouched cell keeps its bytes";
    EXPECT_EQ(dumped(cellAt(merged, 2)), dumped(cellAt(file, 2)));
    const QJsonObject second = cellAt(merged, 1);
    EXPECT_EQ(second.value(QStringLiteral("execution_count")).toInt(), 7);
    EXPECT_EQ(second.value(QStringLiteral("id")).toString(), QStringLiteral("c2"));
    const QJsonArray outputs = second.value(QStringLiteral("outputs")).toArray();
    ASSERT_EQ(outputs.size(), 1);
    EXPECT_EQ(outputs.at(0).toObject().value(QStringLiteral("output_type")).toString(), QStringLiteral("stream"));
    EXPECT_EQ(joinedSource({{QStringLiteral("source"), outputs.at(0).toObject().value(QStringLiteral("text"))}}), QStringLiteral("2\n"));
    expectNbformatValid(merged);

    // Saved, the next read of the same file shows what was run.
    QTemporaryDir otherCache;
    qputenv("XDG_CACHE_HOME", otherCache.path().toUtf8());
    QString reread;
    ASSERT_TRUE(readIpynb(merged, &reread));
    EXPECT_EQ(readNotebookCache(pyKey(QStringLiteral("print(2)"))).outputs.value(0).text, QStringLiteral("2\n"));
    EXPECT_EQ(readNotebookCache(pyKey(QStringLiteral("print(2)"))).executionCount, 7);
}

TEST(IpynbWriteBackTest, ARunOfTheSecondOfTwoIdenticalCellsLeavesTheFirstAlone)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("a"), QStringLiteral("print('x')"), 1, QStringLiteral("old\n")),
                                        codeCell(QStringLiteral("b"), QStringLiteral("print('x')"), 2, QStringLiteral("old\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    writeNotebookCache(pyKey(QStringLiteral("print('x')"), 1), printed(QStringLiteral("new\n"), 9));
    const QByteArray merged = mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("print('x')"), 1)}));
    EXPECT_EQ(dumped(cellAt(merged, 0)), dumped(cellAt(file, 0)));
    EXPECT_EQ(cellAt(merged, 1).value(QStringLiteral("execution_count")).toInt(), 9);
    EXPECT_TRUE(QString::fromUtf8(dumped(cellAt(merged, 1))).contains(QStringLiteral("new")));
}

TEST(IpynbWriteBackTest, ClearingEveryOutputWritesEmptyOutputsAndANullCount)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("c1"), QStringLiteral("print(1)"), 1, QStringLiteral("1\n")),
                                        codeCell(QStringLiteral("c2"), QStringLiteral("print(2)"), 2, QStringLiteral("2\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    const QString one = pyKey(QStringLiteral("print(1)"));
    const QString two = pyKey(QStringLiteral("print(2)"));
    removeNotebookCache(one);
    removeNotebookCache(two);
    const QByteArray merged = mergeIpynb(file, text, runsOf({one, two}));
    for (int i = 0; i < 2; ++i) {
        const QJsonObject cell = cellAt(merged, i);
        EXPECT_TRUE(cell.value(QStringLiteral("outputs")).toArray().isEmpty());
        EXPECT_TRUE(cell.value(QStringLiteral("execution_count")).isNull());
        EXPECT_TRUE(cell.contains(QStringLiteral("execution_count"))) << "nbformat wants the key, as null";
    }
    expectNbformatValid(merged);
}

TEST(IpynbWriteBackTest, WithoutADirtyKeyTheFilesOwnBytesComeBack)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = sampleNotebook();
    IpynbDocument document;
    ASSERT_TRUE(parseIpynb(file, &document));
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    EXPECT_EQ(mergeIpynb(file, text, IpynbRuns()), file);
    EXPECT_EQ(mergeIpynb(file, text), file);
    // A key no cell has changes nothing either.
    EXPECT_EQ(mergeIpynb(file, text, runsOf({QStringLiteral("not a key")})), file);
}

TEST(IpynbWriteBackTest, ADirtyCellInAnUneditedTextIsStillWritten)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("c1"), QStringLiteral("print(1)"), 1, QStringLiteral("1\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    ASSERT_EQ(text, textOf(file));
    writeNotebookCache(pyKey(QStringLiteral("print(1)")), printed(QStringLiteral("changed\n"), 4));
    const QByteArray merged = mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("print(1)"))}));
    EXPECT_NE(merged, file);
    EXPECT_EQ(cellAt(merged, 0).value(QStringLiteral("execution_count")).toInt(), 4);
}

TEST(IpynbWriteBackTest, EveryKindOfOutputRoundTripsThroughTheFile)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("c1"), QStringLiteral("work()"), 0, QString())});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));

    NotebookRunResult result;
    result.executionCount = 5;
    const auto add = [&result](NotebookOutput::Kind kind, const QString &body) {
        NotebookOutput output;
        output.kind = kind;
        output.text = body;
        result.outputs.append(output);
        return &result.outputs.last();
    };
    add(NotebookOutput::Kind::Text, QStringLiteral("out\nline two\n"));
    add(NotebookOutput::Kind::Text, QStringLiteral("warned\n"))->isStderr = true;
    add(NotebookOutput::Kind::Text, QStringLiteral("42"))->isResult = true;
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    add(NotebookOutput::Kind::ImagePng, QString())->imageBytes = png;
    add(NotebookOutput::Kind::ImageSvg, QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));
    NotebookOutput *html = add(NotebookOutput::Kind::Html, QStringLiteral("<b>bold</b>"));
    html->alt = QStringLiteral("bold");
    add(NotebookOutput::Kind::Markdown, QStringLiteral("**md**"));
    NotebookOutput *error = add(NotebookOutput::Kind::Error, QString());
    error->ename = QStringLiteral("ValueError");
    error->evalue = QStringLiteral("bad");
    error->traceback = {QStringLiteral("Traceback"), QStringLiteral("ValueError: bad")};
    result.ok = false;
    const QString key = pyKey(QStringLiteral("work()"));
    writeNotebookCache(key, result);

    const QByteArray merged = mergeIpynb(file, text, runsOf({key}));
    expectNbformatValid(merged);
    const QJsonArray stored = cellAt(merged, 0).value(QStringLiteral("outputs")).toArray();
    ASSERT_EQ(stored.size(), 8);
    EXPECT_EQ(stored.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("stdout"));
    EXPECT_EQ(stored.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("stderr"));
    EXPECT_EQ(stored.at(2).toObject().value(QStringLiteral("output_type")).toString(), QStringLiteral("execute_result"));
    EXPECT_EQ(stored.at(2).toObject().value(QStringLiteral("execution_count")).toInt(), 5);
    EXPECT_EQ(stored.at(3).toObject().value(QStringLiteral("output_type")).toString(), QStringLiteral("display_data"));
    EXPECT_TRUE(stored.at(3).toObject().value(QStringLiteral("data")).toObject().contains(QStringLiteral("image/png")));
    EXPECT_EQ(stored.at(7).toObject().value(QStringLiteral("output_type")).toString(), QStringLiteral("error"));

    // Read back the way a reopened notebook is, they are what was run.
    QTemporaryDir otherCache;
    qputenv("XDG_CACHE_HOME", otherCache.path().toUtf8());
    QString reread;
    ASSERT_TRUE(readIpynb(merged, &reread));
    const NotebookRunResult back = readNotebookCache(key);
    ASSERT_TRUE(back.hasRun);
    EXPECT_EQ(back.executionCount, 5);
    EXPECT_FALSE(back.ok);
    ASSERT_EQ(back.outputs.size(), 8);
    EXPECT_EQ(back.outputs.at(0).text, QStringLiteral("out\nline two\n"));
    EXPECT_TRUE(back.outputs.at(1).isStderr);
    EXPECT_TRUE(back.outputs.at(2).isResult);
    EXPECT_EQ(back.outputs.at(3).imageBytes, png);
    EXPECT_EQ(back.outputs.at(4).kind, NotebookOutput::Kind::ImageSvg);
    EXPECT_EQ(back.outputs.at(5).alt, QStringLiteral("bold"));
    EXPECT_EQ(back.outputs.at(6).text, QStringLiteral("**md**"));
    EXPECT_EQ(back.outputs.at(7).ename, QStringLiteral("ValueError"));
    EXPECT_EQ(back.outputs.at(7).traceback.size(), 2);
}

TEST(IpynbWriteBackTest, ARunThatNeverReachedACellsKernelIsNotWrittenOver)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = notebookOf({codeCell(QStringLiteral("c1"), QStringLiteral("print(1)"), 1, QStringLiteral("1\n"))});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    NotebookRunResult failed;
    failed.kernelError = QStringLiteral("no kernel");
    writeNotebookCache(pyKey(QStringLiteral("print(1)")), failed);
    EXPECT_EQ(mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("print(1)"))})), file);
}

TEST(IpynbWriteBackTest, AFortyPlotNotebookIsStillOpenedWithoutDecodingAnyOutput)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QString png = QStringLiteral(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    QList<QJsonObject> cells;
    for (int i = 0; i < 40; ++i) {
        QJsonObject cell = codeCell(QStringLiteral("c%1").arg(i), QStringLiteral("plot(%1)").arg(i % 8), i + 1, QString());
        cell.insert(QStringLiteral("outputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("output_type"), QStringLiteral("display_data")},
                                           {QStringLiteral("metadata"), QJsonObject()},
                                           {QStringLiteral("data"), QJsonObject{{QStringLiteral("image/png"), png}}}}});
        cells.append(cell); // eight sources, five times each: the ordinals tell them apart
    }
    QString text;
    ASSERT_TRUE(readIpynb(notebookOf(cells), &text));
    EXPECT_TRUE(QDir(notebookCacheRoot()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) << "nothing decoded at read";
    // Each cell is its own entry, looked up on demand.
    const NotebookRunResult last = readNotebookCache(pyKey(QStringLiteral("plot(7)"), 4));
    ASSERT_TRUE(last.hasRun);
    EXPECT_EQ(last.executionCount, 40);
    EXPECT_EQ(last.outputs.value(0).kind, NotebookOutput::Kind::ImagePng);
}

TEST(IpynbNewTest, ANewNotebookIsOneEmptyCodeCellAndTheKernelsOwnSpec)
{
    KernelSpec kernel;
    kernel.name = QStringLiteral("python3");
    kernel.displayName = QStringLiteral("Python 3 (ipykernel)");
    kernel.language = QStringLiteral("python");
    const KernelSpec *const none = nullptr;
    for (const KernelSpec *spec : {static_cast<const KernelSpec *>(&kernel), none}) {
        const QByteArray file = newIpynb(spec);
        IpynbDocument document;
        QString error;
        ASSERT_TRUE(parseIpynb(file, &document, &error)) << error.toStdString();
        EXPECT_EQ(document.root.value(QStringLiteral("nbformat")).toInt(), 4);
        EXPECT_EQ(document.nbformatMinor, 5);
        EXPECT_EQ(document.language, QStringLiteral("python"));
        const QJsonArray cells = document.root.value(QStringLiteral("cells")).toArray();
        ASSERT_EQ(cells.size(), 1);
        const QJsonObject cell = cells.at(0).toObject();
        EXPECT_EQ(cell.value(QStringLiteral("cell_type")).toString(), QStringLiteral("code"));
        EXPECT_FALSE(cell.value(QStringLiteral("id")).toString().isEmpty());
        EXPECT_TRUE(cell.value(QStringLiteral("source")).toArray().isEmpty());
        EXPECT_TRUE(cell.value(QStringLiteral("outputs")).toArray().isEmpty());
        EXPECT_TRUE(cell.value(QStringLiteral("execution_count")).isNull());
        const QJsonObject metadata = document.root.value(QStringLiteral("metadata")).toObject();
        if (spec) {
            EXPECT_EQ(metadata.value(QStringLiteral("kernelspec")).toObject().value(QStringLiteral("name")).toString(),
                      QStringLiteral("python3"));
            EXPECT_FALSE(metadata.contains(QStringLiteral("language_info")));
        } else {
            EXPECT_FALSE(metadata.contains(QStringLiteral("kernelspec")));
            EXPECT_EQ(metadata.value(QStringLiteral("language_info")).toObject().value(QStringLiteral("name")).toString(),
                      QStringLiteral("python"));
        }
        EXPECT_EQ(dumpIpynbJSON(document.root), file) << "written the way nbformat writes it";
        expectNbformatValid(file);
        // It opens as one empty code block.
        QString text;
        ASSERT_TRUE(readIpynb(file, &text));
        EXPECT_EQ(text, QStringLiteral("```python\n\n```\n"));
    }
}

TEST(IpynbAttachmentsTest, AMarkdownCellsAttachmentsComeBackAsImageBytesByCellAndName)
{
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    QJsonObject markdown{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                         {QStringLiteral("id"), QStringLiteral("m")},
                         {QStringLiteral("metadata"), QJsonObject()},
                         {QStringLiteral("source"), QJsonArray{QStringLiteral("![x](attachment:image.png)")}},
                         {QStringLiteral("attachments"),
                          QJsonObject{{QStringLiteral("image.png"),
                                       QJsonObject{{QStringLiteral("image/png"), QString::fromLatin1(png.toBase64())}}},
                                      {QStringLiteral("notes.txt"),
                                       QJsonObject{{QStringLiteral("text/plain"), QStringLiteral("not a picture")}}}}}};
    const QByteArray file = notebookOf({markdown, codeCell(QStringLiteral("c"), QStringLiteral("1"), 0, QString())});

    QString text;
    IpynbPictures pictures;
    ASSERT_TRUE(readIpynb(file, &text, nullptr, &pictures));
    EXPECT_TRUE(text.startsWith(QStringLiteral("![x](attachment:image.png)\n")));
    ASSERT_EQ(pictures.images().size(), 1) << "only pictures";
    EXPECT_EQ(pictures.images().value(QStringLiteral("0/image.png")), png) << "by the cell it is in, then its name";

    // A notebook with none costs an empty table, and reading it without asking is unchanged.
    IpynbPictures none;
    ASSERT_TRUE(readIpynb(sampleNotebook(), &text, nullptr, &none));
    EXPECT_TRUE(none.isEmpty());
    ASSERT_TRUE(readIpynb(file, &text));

    // Saving an edit of that cell keeps what it attached.
    const QByteArray merged = mergeIpynb(file, QStringLiteral("![x](attachment:image.png)\n\nmore\n\n```python\n1\n```\n"));
    EXPECT_EQ(cellAt(merged, 0).value(QStringLiteral("attachments")), markdown.value(QStringLiteral("attachments")));
}

// Two cells that attach different pictures under one name each get their own; which cell a place in an edited
// text is follows the pairing a save makes (a cell inserted above does not hand its neighbour's picture on).
TEST(IpynbAttachmentsTest, EachCellsPictureIsToldApartAndFollowsItsCellThroughEdits)
{
    const auto cell = [](const QString &id, const QString &name, const QByteArray &bytes) {
        return QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("metadata"), QJsonObject()},
                           {QStringLiteral("source"), QJsonArray{QStringLiteral("![](attachment:%1)\n\nof %2").arg(name, id)}},
                           {QStringLiteral("attachments"),
                            QJsonObject{{name, QJsonObject{{QStringLiteral("image/png"), QString::fromLatin1(bytes.toBase64())}}}}}};
    };
    const QByteArray file = notebookOf({cell(QStringLiteral("a"), QStringLiteral("image.png"), "first"),
                                        codeCell(QStringLiteral("c"), QStringLiteral("1"), 0, QString()),
                                        cell(QStringLiteral("b"), QStringLiteral("image.png"), "second")});
    QString text;
    IpynbPictures pictures;
    ASSERT_TRUE(readIpynb(file, &text, nullptr, &pictures));
    EXPECT_EQ(pictures.images().value(QStringLiteral("0/image.png")), QByteArray("first"));
    EXPECT_EQ(pictures.images().value(QStringLiteral("2/image.png")), QByteArray("second"));
    EXPECT_EQ(pictures.scopeAt(text, text.indexOf(QStringLiteral("attachment:"))), QStringLiteral("0/"));
    EXPECT_EQ(pictures.scopeAt(text, text.lastIndexOf(QStringLiteral("attachment:"))), QStringLiteral("2/"));

    // A cell written above the first and a line added in the middle one: the pictures stay with their cells.
    QString edited = QStringLiteral("a new cell first\n\n```python\nprint(0)\n```\n\n") + text;
    edited.replace(QStringLiteral("1\n```"), QStringLiteral("1\nprint(2)\n```"));
    EXPECT_EQ(pictures.scopeAt(edited, edited.indexOf(QStringLiteral("attachment:"))), QStringLiteral("0/"));
    EXPECT_EQ(pictures.scopeAt(edited, edited.lastIndexOf(QStringLiteral("attachment:"))), QStringLiteral("2/"));
    EXPECT_EQ(pictures.scopeAt(edited, 0), QString()) << "the new cell has none";
}

// A code cell deleted and the markdown cell after it edited, with nothing unchanged between them: the gap
// holds a code cell on the old side only, and the markdown cell still is that cell (its id, its pictures).
TEST(IpynbWriteBackTest, DeletingACodeCellBesideAnEditedMarkdownCellKeepsTheMarkdownCellsAttachments)
{
    QJsonObject markdown{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                         {QStringLiteral("id"), QStringLiteral("m")},
                         {QStringLiteral("metadata"), QJsonObject()},
                         {QStringLiteral("source"), QJsonArray{QStringLiteral("a picture")}},
                         {QStringLiteral("attachments"),
                          QJsonObject{{QStringLiteral("a.png"), QJsonObject{{QStringLiteral("image/png"), QStringLiteral("AAAA")}}}}}};
    const QByteArray file = notebookOf({codeCell(QStringLiteral("first"), QStringLiteral("print(0)"), 1, QString()),
                                        codeCell(QStringLiteral("gone"), QStringLiteral("print(1)"), 3, QStringLiteral("1\n")),
                                        markdown});
    const QByteArray merged = mergeIpynb(file, QStringLiteral("```python\nprint(0)\n```\n\na picture, captioned\n"));
    ASSERT_EQ(cellsIn(merged).size(), 2);
    EXPECT_EQ(dumped(cellAt(merged, 0)), dumped(cellAt(file, 0)));
    const QJsonObject kept = cellAt(merged, 1);
    EXPECT_EQ(kept.value(QStringLiteral("id")).toString(), QStringLiteral("m"));
    EXPECT_EQ(kept.value(QStringLiteral("attachments")), markdown.value(QStringLiteral("attachments")));
}

// What was not run is the file's own bytes, not a re-dump of what was parsed from them: the sample is written
// the way Jupyter writes it, so a save after one cell ran differs from it inside that cell alone.
TEST(IpynbWriteBackTest, ARunChangesTheFilesBytesInsideItsCellAlone)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray file = sampleNotebook();
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    removeNotebookCache(pyKey(QStringLiteral("1/0")));
    const QByteArray merged = mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("1/0"))}));
    ASSERT_NE(merged, file);
    const QByteArray open = "  {\n   \"cell_type\": \"code\",\n   \"execution_count\": 4,";
    const qsizetype begin = file.indexOf(open);
    ASSERT_GE(begin, 0);
    const qsizetype end = file.indexOf("\n  }", begin) + 4;
    EXPECT_TRUE(merged.startsWith(file.left(begin)));
    EXPECT_TRUE(merged.endsWith(file.mid(end)));
}

// A display_data that carries only text is not a stream, and a picture keeps its text/plain twin: what a run
// leaves in the file says what the kernel said.
TEST(IpynbWriteBackTest, ADisplayOfTextAndAPicturesTextTwinAreWrittenAsTheKernelSaidThem)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QString png = QStringLiteral(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    QJsonObject cell = codeCell(QStringLiteral("c"), QStringLiteral("show()"), 2, QString());
    cell.insert(QStringLiteral("outputs"),
                QJsonArray{QJsonObject{{QStringLiteral("output_type"), QStringLiteral("display_data")},
                                       {QStringLiteral("metadata"), QJsonObject()},
                                       {QStringLiteral("data"), QJsonObject{{QStringLiteral("text/plain"), QStringLiteral("<Figure>")}}}},
                           QJsonObject{{QStringLiteral("output_type"), QStringLiteral("display_data")},
                                       {QStringLiteral("metadata"), QJsonObject()},
                                       {QStringLiteral("data"), QJsonObject{{QStringLiteral("image/png"), png},
                                                                            {QStringLiteral("text/plain"), QStringLiteral("<Figure size 1x1>")}}}}});
    const QByteArray file = notebookOf({cell});
    QString text;
    ASSERT_TRUE(readIpynb(file, &text));
    const QByteArray merged = mergeIpynb(file, text, runsOf({pyKey(QStringLiteral("show()"))}));
    const QJsonArray outputs = cellAt(merged, 0).value(QStringLiteral("outputs")).toArray();
    ASSERT_EQ(outputs.size(), 2);
    EXPECT_EQ(outputs.at(0).toObject().value(QStringLiteral("output_type")).toString(), QStringLiteral("display_data"));
    EXPECT_TRUE(outputs.at(0).toObject().value(QStringLiteral("data")).toObject().contains(QStringLiteral("text/plain")));
    const QJsonObject picture = outputs.at(1).toObject().value(QStringLiteral("data")).toObject();
    EXPECT_TRUE(picture.contains(QStringLiteral("image/png")));
    EXPECT_TRUE(picture.contains(QStringLiteral("text/plain"))) << "the picture's text twin is kept";
    expectNbformatValid(merged);
}

TEST(IpynbWriteBackTest, EditingTheLastCellKeepsItsIDOutputsAndAttachments)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QJsonObject markdown{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                         {QStringLiteral("id"), QStringLiteral("m")},
                         {QStringLiteral("metadata"), QJsonObject()},
                         {QStringLiteral("source"), QJsonArray{QStringLiteral("last words")}},
                         {QStringLiteral("attachments"),
                          QJsonObject{{QStringLiteral("a.png"), QJsonObject{{QStringLiteral("image/png"), QStringLiteral("AAAA")}}}}}};
    for (const bool lastIsCode : {false, true}) {
        const QByteArray file = lastIsCode
                                    ? notebookOf({markdown, codeCell(QStringLiteral("c"), QStringLiteral("print(1)"), 3, QStringLiteral("1\n"))})
                                    : notebookOf({codeCell(QStringLiteral("c"), QStringLiteral("print(1)"), 3, QStringLiteral("1\n")), markdown});
        QString text = textOf(file);
        text.replace(lastIsCode ? QStringLiteral("print(1)") : QStringLiteral("last words"),
                     lastIsCode ? QStringLiteral("print(11)") : QStringLiteral("last words, edited"));
        const QByteArray merged = mergeIpynb(file, text);
        const QJsonObject edited = cellAt(merged, 1);
        const QJsonObject kept = cellAt(file, 1);
        EXPECT_EQ(edited.value(QStringLiteral("id")), kept.value(QStringLiteral("id"))) << lastIsCode;
        EXPECT_EQ(edited.value(QStringLiteral("attachments")), kept.value(QStringLiteral("attachments"))) << lastIsCode;
        EXPECT_EQ(edited.value(QStringLiteral("outputs")), kept.value(QStringLiteral("outputs"))) << lastIsCode;
        EXPECT_EQ(dumped(cellAt(merged, 0)), dumped(cellAt(file, 0))) << "the first is the file's own";
    }
}

// --- what a cell can see and do ----------------------------------------------------

namespace {

bool sandboxUsable()
{
    // Whether a trivial cell runs: bwrap installed, user namespaces allowed here, libseccomp built in.
    const auto result = runDirect(QStringLiteral("bash"), QStringLiteral("echo ok\n"));
    return result && result->ok && textOf(*result).trimmed() == QStringLiteral("ok");
}

} // namespace

TEST(DirectRunnerTest, ACellSeesNeitherTheHostsProcessesNorTheHomeDirectory)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    // Its own pid namespace: /proc lists the cell's own few processes, never this one, and no pid of the host.
    const QString script = QStringLiteral(
        "n=0; for p in /proc/[0-9]*; do n=$((n+1)); done; echo count=$n\n"
        "[ -e /proc/%1 ] && echo sees-the-app || echo does-not-see-the-app\n"
        "kill -0 %1 2>/dev/null && echo can-signal-the-app || echo cannot-signal-the-app\n"
        "ls '%2' >/dev/null 2>&1 && echo home-visible || echo home-hidden\n"
        "ls /home >/dev/null 2>&1 && echo slash-home-visible || echo slash-home-hidden\n")
                               .arg(QCoreApplication::applicationPid())
                               .arg(QDir::homePath());
    const auto result = runDirect(QStringLiteral("bash"), script);
    ASSERT_TRUE(result.has_value());
    const QString text = textOf(*result);
    EXPECT_TRUE(text.contains(QStringLiteral("does-not-see-the-app"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("cannot-signal-the-app"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("home-hidden"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("slash-home-hidden"))) << text.toStdString();
    static const QRegularExpression count(QStringLiteral("count=(\\d+)"));
    const QRegularExpressionMatch match = count.match(text);
    ASSERT_TRUE(match.hasMatch()) << text.toStdString();
    EXPECT_LT(match.captured(1).toInt(), 10) << "only the cell's own processes";
}

TEST(DirectRunnerTest, ACellCannotReachTheNetworkOrTheHostEnvironment)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    qputenv("NOTEAPPC_TEST_SECRET", "hunter2");
    const auto result = runDirect(QStringLiteral("bash"),
                                  QStringLiteral("echo \"[$NOTEAPPC_TEST_SECRET]\"; ls /sys/class/net | tr '\\n' ' '; echo\n"
                                                 "cat /proc/net/dev | wc -l\n"));
    qunsetenv("NOTEAPPC_TEST_SECRET");
    ASSERT_TRUE(result.has_value());
    const QString text = textOf(*result);
    EXPECT_TRUE(text.contains(QStringLiteral("[]"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("hunter2")));
    // Only the loopback (down): no interface of the host.
    EXPECT_FALSE(text.contains(QStringLiteral("eth")) || text.contains(QStringLiteral("wl")) || text.contains(QStringLiteral("enp")));
}

TEST(DirectRunnerTest, TheSyscallFilterAnswersPermissionDeniedToTheCallsThatLeaveASandbox)
{
#if defined(__x86_64__)
    if (!DirectRunner::canRun(QStringLiteral("python")) || !sandboxUsable())
        GTEST_SKIP() << "python or a usable bubblewrap is not available";
    // Raw syscalls by number (x86-64): ptrace, mount, unshare, setns, keyctl, bpf, userfaultfd, perf_event_open,
    // io_uring_setup, kexec_load, init_module, personality with a changed domain.
    const auto result = runDirect(QStringLiteral("python"), QStringLiteral(
        "import ctypes\n"
        "libc = ctypes.CDLL(None, use_errno=True)\n"
        "names = {101: 'ptrace', 165: 'mount', 272: 'unshare', 308: 'setns', 250: 'keyctl', 321: 'bpf',\n"
        "         323: 'userfaultfd', 298: 'perf_event_open', 425: 'io_uring_setup', 246: 'kexec_load',\n"
        "         175: 'init_module', 135: 'personality'}\n"
        "for number, name in names.items():\n"
        "    ctypes.set_errno(0)\n"
        "    argument = 0x0040000 if name == 'personality' else 0\n"
        "    result = libc.syscall(number, argument, 0, 0, 0, 0, 0)\n"
        "    print(name, result, ctypes.get_errno())\n"
        "ctypes.set_errno(0)\n"
"print('personality-default', libc.syscall(135, 0) >= 0)\n"));
    ASSERT_TRUE(result.has_value());
    const QString text = textOf(*result);
    for (const char *name : {"ptrace", "mount", "unshare", "setns", "keyctl", "bpf", "userfaultfd", "perf_event_open",
                             "io_uring_setup", "kexec_load", "init_module", "personality"})
        EXPECT_TRUE(text.contains(QStringLiteral("%1 -1 1\n").arg(QLatin1String(name)))) << name << "\n" << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("personality-default True"))) << text.toStdString();
#else
    GTEST_SKIP() << "the syscall numbers here are x86-64's";
#endif
}

TEST(DirectRunnerTest, ACellCannotMakeASocketOfAFamilyItHasNoUseFor)
{
    if (!DirectRunner::canRun(QStringLiteral("python")) || !sandboxUsable())
        GTEST_SKIP() << "python or a usable bubblewrap is not available";
    // AF_VSOCK 40, AF_ALG 38, AF_KEY 15, AF_PACKET 17, AF_RDS 21, AF_TIPC 30, AF_XDP 44, AF_BLUETOOTH 31, and
    // AF_NETLINK 16 with a protocol other than NETLINK_ROUTE (0), which getifaddrs() and so libzmq need.
    const auto result = runDirect(QStringLiteral("python"), QStringLiteral(
        "import socket\n"
        "for name, family, protocol in [('vsock', 40, 0), ('alg', 38, 0), ('key', 15, 0), ('packet', 17, 0),\n"
        "                               ('rds', 21, 0), ('tipc', 30, 0), ('xdp', 44, 0), ('bluetooth', 31, 0),\n"
        "                               ('netfilter', 16, 12), ('audit', 16, 9), ('crypto', 16, 21)]:\n"
        "    try:\n"
        "        socket.socket(family, socket.SOCK_RAW if family in (17, 16) else socket.SOCK_STREAM, protocol)\n"
        "        print(name, 'created')\n"
        "    except OSError as error:\n"
        "        print(name, error.errno)\n"
        "socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
        "socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
        "socket.socket(16, socket.SOCK_RAW, 0)\n"
        "print('unix-inet-and-netlink-route-ok', len(socket.if_nameindex()) > 0)\n"));
    ASSERT_TRUE(result.has_value());
    const QString text = textOf(*result);
    for (const char *name : {"vsock", "alg", "key", "packet", "rds", "tipc", "xdp", "bluetooth", "netfilter", "audit", "crypto"})
        EXPECT_TRUE(text.contains(QStringLiteral("%1 97\n").arg(QLatin1String(name)))) << name << "\n" << text.toStdString(); // EAFNOSUPPORT
    EXPECT_TRUE(text.contains(QStringLiteral("unix-inet-and-netlink-route-ok True"))) << text.toStdString();
}

TEST(DirectRunnerTest, ACellKeepsTheSyscallFilterAndSeesOnlyItsOwnAccountFiles)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    // Through the scope too (when there is one): the filter's descriptor must reach bwrap by way of systemd-run.
    const auto result = runDirect(QStringLiteral("bash"), QStringLiteral(
        "grep '^Seccomp:' /proc/self/status\n"
        "echo passwd-lines=$(wc -l < /etc/passwd) group-lines=$(wc -l < /etc/group)\n"
        "echo me=$(id -un) uid=$(id -u)\n"
        "grep -c . /etc/passwd /etc/group\n"));
    ASSERT_TRUE(result.has_value());
    const QString text = textOf(*result);
    EXPECT_TRUE(text.contains(QRegularExpression(QStringLiteral("Seccomp:\\s+2")))) << text.toStdString();
    // One line each, the cell's own: not the host's list of accounts (root, daemon, other users...).
    EXPECT_TRUE(text.contains(QStringLiteral("passwd-lines=1 group-lines=1"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("me=user uid=%1").arg(getuid()))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("root"))) << text.toStdString();
}

TEST(DirectRunnerTest, ACellCannotFillTheMachinesMemoryThroughDevShmOrItsOwnDirectory)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !DirectRunner::canRun(QStringLiteral("python")) || !sandboxUsable())
        GTEST_SKIP() << "bash, python or a usable bubblewrap is not available";
    // /dev/shm is a tmpfs of 64 MiB, like /tmp: the hundred-megabyte write stops short.
    const auto shm = runDirect(QStringLiteral("bash"), QStringLiteral(
        "echo full=$(dd if=/dev/zero of=/dev/shm/big bs=1M count=100 2>&1 | grep -c 'No space left')\n"
        "echo size=$(stat -c %s /dev/shm/big)\n"));
    ASSERT_TRUE(shm.has_value());
    const QString shmText = textOf(*shm);
    EXPECT_TRUE(shmText.contains(QStringLiteral("full=1"))) << shmText.toStdString();
    static const QRegularExpression size(QStringLiteral("\\bsize=(\\d+)"));
    const QRegularExpressionMatch match = size.match(shmText);
    ASSERT_TRUE(match.hasMatch()) << shmText.toStdString();
    EXPECT_LE(match.captured(1).toLongLong(), 64LL << 20);
    EXPECT_GT(match.captured(1).toLongLong(), 0);

    // Memory itself, past the cgroup's 1 GiB, ends the cell (the per-process limit is 4 GiB: it is not that).
    if (!noteAppC::sandbox::cgroupLimitsAvailable())
        GTEST_SKIP() << "no systemd user manager: only the rlimits cap this";
    const auto memory = runDirect(QStringLiteral("python"), QStringLiteral(
        "block = b'x' * (1800 << 20)\nprint('survived', len(block))\n"));
    ASSERT_TRUE(memory.has_value());
    EXPECT_FALSE(memory->ok) << textOf(*memory).toStdString();
    EXPECT_FALSE(textOf(*memory).contains(QStringLiteral("survived")));
    // And files in the cell's own directory count (a tmpfs there is memory): six of 200 MiB.
    if (QStorageInfo(QDir::tempPath()).fileSystemType() != "tmpfs")
        GTEST_SKIP() << "the temporary directory is not a tmpfs here";
    const auto files = runDirect(QStringLiteral("bash"), QStringLiteral(
        "for i in 1 2 3 4 5 6 7 8; do dd if=/dev/zero of=big$i bs=1M count=200 2>/dev/null; done\n"
        "echo total=$(cat big* | wc -c)\n"));
    ASSERT_TRUE(files.has_value());
    // The shell may be the one that was killed (no total at all); if it printed one, it is under the 1 GiB cap
    // (8 x 200 MiB were asked for).
    static const QRegularExpression total(QStringLiteral("total=(\\d+)"));
    const QRegularExpressionMatch filesMatch = total.match(textOf(*files));
    if (filesMatch.hasMatch())
        EXPECT_LE(filesMatch.captured(1).toLongLong(), (1LL << 30) + (64LL << 20)) << textOf(*files).toStdString();
}

TEST(DirectRunnerTest, ACellRunsWithoutACgroupWhenThereIsNoUserManager)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    noteAppC::sandbox::overrideCgroupLimits(false);
    const auto result = runDirect(QStringLiteral("bash"), QStringLiteral("echo still-runs\n"));
    noteAppC::sandbox::forgetCgroupProbe();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok);
    EXPECT_EQ(textOf(*result).trimmed(), QStringLiteral("still-runs"));
}

// A test run on a machine that can run the sandbox must run it: the tests above skip when a trivial cell does
// not run, which on a CI box without user namespaces would leave all of them green and the sandbox untested.
TEST(NotebookSandboxTest, WhereBubblewrapAndUserNamespacesAreThereACellRuns)
{
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "bubblewrap not installed";
    if (!DirectRunner::canRun(QStringLiteral("bash")))
        GTEST_SKIP() << "bash not installed";
    // Whether this process may make a user namespace, asked of a child so that this one is untouched.
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
        _exit(unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        GTEST_SKIP() << "user namespaces are not allowed here";
    QStringList probe = {QStringLiteral("--"), QStringLiteral("x")};
    const int filter = noteAppC::sandbox::addSyscallFilter(probe);
    if (filter < 0)
        GTEST_SKIP() << "built without libseccomp";
    close(filter);
    EXPECT_TRUE(sandboxUsable()) << "bubblewrap is installed and user namespaces work, yet a trivial cell did not run: "
                                    "every sandbox test is skipped on this machine, so none of them says anything";
}

TEST(DirectRunnerTest, ACellIsHeldToItsMemoryAndProcessLimits)
{
    if (!DirectRunner::canRun(QStringLiteral("python")) || !sandboxUsable())
        GTEST_SKIP() << "python or a usable bubblewrap is not available";
    const auto memory = runDirect(QStringLiteral("python"), QStringLiteral(
        "try:\n    block = bytearray(16 << 30)\n    print('allocated')\nexcept MemoryError:\n    print('refused')\n"));
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(textOf(*memory).trimmed(), QStringLiteral("refused"));
    const auto forks = runDirect(QStringLiteral("python"), QStringLiteral(
        "import os, time\nn = 0\ntry:\n    while n < 5000:\n        if os.fork() == 0:\n            time.sleep(20)\n            os._exit(0)\n"
        "        n += 1\nexcept OSError:\n    pass\nprint('forked', n)\n"));
    ASSERT_TRUE(forks.has_value());
    static const QRegularExpression forked(QStringLiteral("forked (\\d+)"));
    const QRegularExpressionMatch match = forked.match(textOf(*forks));
    ASSERT_TRUE(match.hasMatch()) << textOf(*forks).toStdString();
    EXPECT_LT(match.captured(1).toInt(), 300) << "a fork bomb stops at the process limit";
    EXPECT_GT(match.captured(1).toInt(), 1);
    // Output beyond the cap ends the run, on either channel.
    const auto flood = runDirect(QStringLiteral("bash"), QStringLiteral("yes >&2\n"), SandboxTier::Bubblewrap, 20'000);
    ASSERT_TRUE(flood.has_value());
    EXPECT_FALSE(flood->ok);
    EXPECT_LT(flood->wallMs, 15'000);
}

TEST(DirectRunnerTest, ABlocksOwnTimeoutIsClamped)
{
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    // `#| timeout: 86400` is honoured only up to five minutes: this checks the setting is read, not waited for.
    DirectRunner runner;
    DirectRunner::Request request;
    request.language = QStringLiteral("bash");
    request.source = QStringLiteral("sleep 0.1\n");
    request.timeoutMs = 86'400'000;
    std::optional<NotebookRunResult> result;
    runner.run(request, [&result](NotebookRunResult r) { result = r; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, 30'000));
    EXPECT_TRUE(result->ok);
}

TEST(NotebookSandboxTest, ABigBlockIsNotRun)
{
    EXPECT_TRUE(oversizeMessage(QString(1000, QLatin1Char('x')), 0).isEmpty()) << "0 lifts the limit";
    EXPECT_TRUE(oversizeMessage(QString(1000, QLatin1Char('x')), 1000).isEmpty());
    const QString message = oversizeMessage(QString(65537, QLatin1Char('x')), 65536);
    EXPECT_FALSE(message.isEmpty());
    EXPECT_TRUE(message.contains(QStringLiteral("notebooks.maxRunnableBytes")));
    // Bytes, not characters.
    EXPECT_FALSE(oversizeMessage(QString(600, QChar(0x00e9)), 1000).isEmpty());
}

TEST(NotebookSandboxTest, ARunnableEmbedsFolderIsBoundOnlyInsideTheVault)
{
    QTemporaryDir vault;
    QTemporaryDir elsewhere;
    ASSERT_TRUE(vault.isValid() && elsewhere.isValid());
    QDir(vault.path()).mkdir(QStringLiteral("scripts"));
    const QString inside = vault.filePath(QStringLiteral("scripts"));
    EXPECT_EQ(confinedBind(inside, vault.path()), QFileInfo(inside).canonicalFilePath());
    EXPECT_TRUE(confinedBind(elsewhere.path(), vault.path()).isEmpty());
    // A symlink in the vault pointing out of it.
    const QString link = vault.filePath(QStringLiteral("out"));
    ASSERT_TRUE(QFile::link(elsewhere.path(), link));
    EXPECT_TRUE(confinedBind(link, vault.path()).isEmpty());
    // A vault that is the whole home directory or the disk hands nothing over.
    EXPECT_TRUE(confinedBind(QDir::homePath(), QDir::homePath()).isEmpty());
    EXPECT_TRUE(confinedBind(QStringLiteral("/usr"), QStringLiteral("/")).isEmpty());
}

TEST(KernelProcessTest, InterruptReachesTheProcessInsideThePidNamespace)
{
    if (!sandboxUsable())
        GTEST_SKIP() << "a usable bubblewrap is not available";
    QTemporaryDir runtimeRoot;
    ASSERT_TRUE(runtimeRoot.isValid());
    qputenv("JUPYTER_RUNTIME_DIR", runtimeRoot.path().toUtf8());
    KernelSpec spec;
    spec.name = QStringLiteral("interruptible");
    spec.argv = {QStringLiteral("python3"), QStringLiteral("-c"),
                 QStringLiteral("import os, signal, sys, time\n"
                                "def stop(*a):\n    os._exit(7)\n"
                                "signal.signal(signal.SIGINT, stop)\n"
                                "open(os.path.join(os.path.dirname(sys.argv[1]), 'ready'), 'w').write(str(os.getpid()))\n"
                                "time.sleep(60)\n"),
                 QStringLiteral("{connection_file}")};
    spec.argv[0] = QStandardPaths::findExecutable(QStringLiteral("python3"));
    KernelProcess process;
    if (spec.argv[0].isEmpty() || !process.start(spec, SandboxTier::Bubblewrap, {}))
        GTEST_SKIP() << process.error().toStdString();
    int exitCode = -1;
    bool finished = false;
    QObject::connect(&process, &KernelProcess::finished, [&](int code) { exitCode = code; finished = true; });
    const QString ready = QFileInfo(process.connectionInfo().ip).absolutePath() + QStringLiteral("/ready");
    ASSERT_TRUE(QTest::qWaitFor([&] { return QFileInfo::exists(ready) || finished; }, 20'000));
    ASSERT_FALSE(finished);
    // It is pid 2 of its own namespace (bwrap's init is 1): a number no host process has been given by the
    // namespace's own count, and not the one QProcess started.
    QFile pidFile(ready);
    ASSERT_TRUE(pidFile.open(QIODevice::ReadOnly));
    EXPECT_LT(pidFile.readAll().toInt(), 100);
    process.interrupt();
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished; }, 10'000));
    EXPECT_EQ(exitCode, 7) << "SIGINT arrived in the cell, not at the wrapper";
    qunsetenv("JUPYTER_RUNTIME_DIR");
}

// --- the kernel's sockets are the kernel's to make, and the app's to check -----------------

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

namespace {

/// A unix socket listening at `path`, nonblocking: the stand-in for something of the user's the app must never
/// connect to (the session bus, an agent), or for one of a kernel's five.
int listenAt(const QString &path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, QFile::encodeName(path).constData(), sizeof(address.sun_path) - 1);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 8) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

/// How many connections have arrived at `listening` since the last call.
int arrivals(int listening)
{
    int count = 0;
    for (;;) {
        const int accepted = accept4(listening, nullptr, nullptr, SOCK_NONBLOCK);
        if (accepted < 0)
            return count;
        close(accepted);
        ++count;
    }
}

KernelConnectionInfo infoIn(const QString &dir)
{
    KernelConnectionInfo info;
    info.ip = dir + QStringLiteral("/kernel");
    info.key = QByteArrayLiteral("secret");
    return info;
}

} // namespace

TEST(KernelClientTest, AnEndpointIsCheckedWithoutFollowingALink)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString real = dir.filePath(QStringLiteral("real"));
    const int server = listenAt(real);
    ASSERT_GE(server, 0);
    ASSERT_TRUE(QFile::link(real, dir.filePath(QStringLiteral("link"))));
    ASSERT_TRUE(QFile::link(dir.filePath(QStringLiteral("nowhere")), dir.filePath(QStringLiteral("dangling"))));
    QFile plain(dir.filePath(QStringLiteral("plain")));
    ASSERT_TRUE(plain.open(QIODevice::WriteOnly));
    plain.close();
    const auto state = [&](const char *name) { return checkEndpoint(QStringLiteral("ipc://") + dir.filePath(QLatin1String(name))); };
    EXPECT_EQ(state("real"), EndpointState::Plain);
    EXPECT_EQ(state("link"), EndpointState::Refused) << "a link to a socket is not a socket";
    EXPECT_EQ(state("dangling"), EndpointState::Refused);
    EXPECT_EQ(state("plain"), EndpointState::Refused);
    EXPECT_EQ(state("absent"), EndpointState::Missing);
    EXPECT_EQ(checkEndpoint(QStringLiteral("tcp://127.0.0.1:5555")), EndpointState::Plain) << "nothing to check";
    close(server);
}

TEST(KernelClientTest, ALinkPlantedWhereASocketBelongsIsNeverConnectedTo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const KernelConnectionInfo info = infoIn(dir.path());
    // Something of the user's, outside the kernel's directory, that the kernel's "shell" socket now points at.
    QTemporaryDir elsewhere;
    ASSERT_TRUE(elsewhere.isValid());
    const int target = listenAt(elsewhere.filePath(QStringLiteral("bus")));
    ASSERT_GE(target, 0);
    QList<int> sockets;
    for (const int port : {info.iopubPort, info.stdinPort, info.controlPort, info.hbPort}) {
        sockets << listenAt(dir.filePath(QStringLiteral("kernel-%1").arg(port)));
        ASSERT_GE(sockets.last(), 0);
    }
    ASSERT_TRUE(QFile::link(elsewhere.filePath(QStringLiteral("bus")), dir.filePath(QStringLiteral("kernel-%1").arg(info.shellPort))));

    KernelClient client;
    client.startClient(info, QStringLiteral("session"), QStringLiteral("user"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isFinished(); }, 5000)) << "the client stops at a link";
    QTest::qWait(300);
    EXPECT_EQ(arrivals(target), 0) << "the app connected through a link";
    for (const int socket : std::as_const(sockets))
        EXPECT_EQ(arrivals(socket), 0) << "it connects to none of the five once one is refused";
    for (const int socket : std::as_const(sockets))
        close(socket);
    close(target);
}

TEST(KernelClientTest, ASocketSwappedForALinkAfterTheConnectIsNotFollowedOnReconnect)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const KernelConnectionInfo info = infoIn(dir.path());
    QTemporaryDir elsewhere;
    ASSERT_TRUE(elsewhere.isValid());
    const int target = listenAt(elsewhere.filePath(QStringLiteral("bus")));
    ASSERT_GE(target, 0);
    QHash<int, int> sockets; // by port
    for (const int port : {info.shellPort, info.iopubPort, info.stdinPort, info.controlPort, info.hbPort}) {
        sockets[port] = listenAt(dir.filePath(QStringLiteral("kernel-%1").arg(port)));
        ASSERT_GE(sockets[port], 0);
    }
    KernelClient client;
    client.startClient(info, QStringLiteral("session"), QStringLiteral("user"));
    // It is connected to the real shell socket once a connection arrives there.
    int shellArrivals = 0;
    ASSERT_TRUE(QTest::qWaitFor([&] { return (shellArrivals += arrivals(sockets[info.shellPort])) > 0; }, 5000));

    // The kernel closes its shell socket and puts a link to the target in its place.
    const QString shell = dir.filePath(QStringLiteral("kernel-%1").arg(info.shellPort));
    close(sockets[info.shellPort]);
    ASSERT_TRUE(QFile::remove(shell));
    ASSERT_TRUE(QFile::link(elsewhere.filePath(QStringLiteral("bus")), shell));
    ASSERT_TRUE(QTest::qWaitFor([&] { return client.isFinished(); }, 5000)) << "the client sees the link and disconnects";
    QTest::qWait(1500); // past libzmq's reconnect interval
    EXPECT_EQ(arrivals(target), 0) << "the app followed the link";
    for (auto it = sockets.cbegin(); it != sockets.cend(); ++it) {
        if (it.key() != info.shellPort)
            close(it.value());
    }
    close(target);
}

// --- the one place every run passes -------------------------------------------------------

TEST(NotebookControllerTest, TheSizeLimitAndTheRunGateAreAskedByRunItselfForEveryCaller)
{
    QTemporaryDir marks;
    ASSERT_TRUE(marks.isValid());
    const QString mark = marks.filePath(QStringLiteral("ran"));
    NotebookController controller;
    const auto ask = [&](const QString &source, bool forceRun) {
        NotebookController::RunRequest request;
        request.language = QStringLiteral("bash");
        request.source = source;
        request.cache = false;
        std::optional<NotebookRunResult> answer;
        controller.run(request, forceRun, [&answer](NotebookRunResult r) { answer = r; });
        (void)QTest::qWaitFor([&] { return answer.has_value(); }, 30'000);
        return answer;
    };

    // Over the default limit (64 KiB): refused, with the setting's name, and nothing started.
    const QString big = QStringLiteral("touch '%1'\n#").arg(mark) + QString(70'000, QLatin1Char('x'));
    auto refused = ask(big, true);
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->hasRun);
    EXPECT_TRUE(refused->kernelError.contains(QStringLiteral("notebooks.maxRunnableBytes"))) << refused->kernelError.toStdString();
    EXPECT_FALSE(QFileInfo::exists(mark));

    // A gate that says no: the message is the block's error, and nothing runs. It sees the request.
    QStringList seen;
    controller.setRunGate([&seen](const NotebookController::RunRequest &request) {
        seen << request.language;
        return QStringLiteral("Not run: a test said no");
    });
    refused = ask(QStringLiteral("touch '%1'\n").arg(mark), true);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->kernelError, QStringLiteral("Not run: a test said no"));
    EXPECT_EQ(seen, QStringList{QStringLiteral("bash")});
    EXPECT_FALSE(QFileInfo::exists(mark));

    // A cache lookup is neither run nor asked about.
    const auto lookup = ask(big, false);
    ASSERT_TRUE(lookup.has_value());
    EXPECT_TRUE(lookup->kernelError.isEmpty());
    EXPECT_EQ(seen.size(), 1);

    // A gate that says yes lets the run go on, in the sandbox. The marker is written where the cell can see it:
    // its own temporary directory is the only writable place, so ask for output instead.
    controller.setRunGate([](const NotebookController::RunRequest &) { return QString(); });
    if (!DirectRunner::canRun(QStringLiteral("bash")) || !sandboxUsable())
        GTEST_SKIP() << "bash or a usable bubblewrap is not available";
    const auto allowed = ask(QStringLiteral("echo allowed\n"), true);
    ASSERT_TRUE(allowed.has_value());
    EXPECT_TRUE(allowed->kernelError.isEmpty()) << allowed->kernelError.toStdString();
    EXPECT_EQ(textOf(*allowed).trimmed(), QStringLiteral("allowed"));
}
