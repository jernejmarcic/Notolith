// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "DirectRunner.h"

#include "Bubblewrap.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

#include <unistd.h>

namespace noteAppC::notebook {

namespace {

/// How a language runs: the file its source is written to, the tool that
/// must be installed, and the command line, run in the block's own
/// temporary directory. `%f` is the file, `%d` the directory.
struct Recipe {
    QStringList names;
    QString file;
    QString tool;
    QStringList command;
};

// Python prints the value of a last bare expression, as a notebook cell does.
constexpr char kPythonCell[] = R"(import ast, sys
path = sys.argv[1]
tree = ast.parse(open(path).read(), path)
last = tree.body.pop() if tree.body and isinstance(tree.body[-1], ast.Expr) else None
scope = {"__name__": "__main__"}
exec(compile(tree, path, "exec"), scope)
if last is not None:
    value = eval(compile(ast.Expression(last.value), path, "eval"), scope)
    if value is not None:
        print(repr(value))
)";

const QList<Recipe> &recipes()
{
    static const QList<Recipe> list = {
        {{QStringLiteral("python"), QStringLiteral("python3"), QStringLiteral("py")}, QStringLiteral("cell.py"),
         QStringLiteral("python3"), {QStringLiteral("python3"), QStringLiteral("-c"), QString::fromLatin1(kPythonCell), QStringLiteral("%f")}},
        // Haskell through ghci, which prints every expression's value; a
        // block with a `main` is a program and goes to runghc instead.
        {{QStringLiteral("haskell"), QStringLiteral("hs")}, QStringLiteral("cell.ghci"), QStringLiteral("ghci"),
         {QStringLiteral("ghci"), QStringLiteral("-v0"), QStringLiteral("-ignore-dot-ghci"), QStringLiteral("-ghci-script"), QStringLiteral("%f"), QStringLiteral("-e"), QStringLiteral("return ()")}},
        {{QStringLiteral("bash"), QStringLiteral("sh"), QStringLiteral("shell"), QStringLiteral("zsh")}, QStringLiteral("cell.sh"),
         QStringLiteral("bash"), {QStringLiteral("bash"), QStringLiteral("%f")}},
        {{QStringLiteral("javascript"), QStringLiteral("js"), QStringLiteral("node")}, QStringLiteral("cell.js"),
         QStringLiteral("node"), {QStringLiteral("node"), QStringLiteral("%f")}},
        {{QStringLiteral("ruby"), QStringLiteral("rb")}, QStringLiteral("cell.rb"), QStringLiteral("ruby"), {QStringLiteral("ruby"), QStringLiteral("%f")}},
        {{QStringLiteral("perl"), QStringLiteral("pl")}, QStringLiteral("cell.pl"), QStringLiteral("perl"), {QStringLiteral("perl"), QStringLiteral("%f")}},
        {{QStringLiteral("lua")}, QStringLiteral("cell.lua"), QStringLiteral("lua"), {QStringLiteral("lua"), QStringLiteral("%f")}},
        {{QStringLiteral("c")}, QStringLiteral("cell.c"), QStringLiteral("gcc"),
         {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("gcc -O1 -o \"$1/cell\" \"$1/cell.c\" -lm && \"$1/cell\""), QStringLiteral("sh"), QStringLiteral("%d")}},
        {{QStringLiteral("cpp"), QStringLiteral("c++"), QStringLiteral("cxx")}, QStringLiteral("cell.cpp"), QStringLiteral("g++"),
         {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("g++ -std=c++20 -O1 -o \"$1/cell\" \"$1/cell.cpp\" && \"$1/cell\""), QStringLiteral("sh"), QStringLiteral("%d")}},
        {{QStringLiteral("rust"), QStringLiteral("rs")}, QStringLiteral("cell.rs"), QStringLiteral("rustc"),
         {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("rustc -O -o \"$1/cell\" \"$1/cell.rs\" && \"$1/cell\""), QStringLiteral("sh"), QStringLiteral("%d")}},
        {{QStringLiteral("java")}, QStringLiteral("Main.java"), QStringLiteral("java"), {QStringLiteral("java"), QStringLiteral("%f")}},
        // WebAssembly text, run by wasmtime. Both names take WAT: a binary
        // module cannot live in a Markdown fence. The tool is how the WASM
        // tier tells a block that is the module from one that needs one.
        {{QStringLiteral("wat"), QStringLiteral("wasm")}, QStringLiteral("cell.wat"), QStringLiteral("wasmtime"),
         {QStringLiteral("wasmtime"), QStringLiteral("run"), QStringLiteral("-C"), QStringLiteral("cache=n"), QStringLiteral("%f")}},
    };
    return list;
}

const Recipe *recipeFor(const QString &language)
{
    const QString wanted = language.trimmed().toLower();
    for (const Recipe &recipe : recipes()) {
        if (recipe.names.contains(wanted))
            return &recipe;
    }
    return nullptr;
}

/// A Haskell block as a ghci script: each top-level declaration or
/// expression one input, a multi-line one wrapped in :{ :} so ghci reads it
/// whole.
QString ghciScript(const QString &source)
{
    QStringList chunks;
    QStringList current;
    const auto flush = [&] {
        while (!current.isEmpty() && current.last().trimmed().isEmpty())
            current.removeLast();
        if (current.size() == 1)
            chunks << current.first();
        else if (!current.isEmpty())
            chunks << QStringLiteral(":{\n") + current.join(QLatin1Char('\n')) + QStringLiteral("\n:}");
        current.clear();
    };
    for (const QString &line : source.split(QLatin1Char('\n'))) {
        const bool continues = line.isEmpty() || line.at(0).isSpace();
        if (!continues)
            flush();
        if (!current.isEmpty() || !line.trimmed().isEmpty())
            current << line;
    }
    flush();
    return chunks.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

struct Check {
    const char *tool;
    const char *entry;
    const char *command;
};
const Check checks[] = {
    {"gcc", "\\bmain\\s*\\(", "gcc -fsyntax-only \"$1/cell.c\""},
    {"g++", "\\bmain\\s*\\(", "g++ -std=c++20 -fsyntax-only \"$1/cell.cpp\""},
    {"rustc", "\\bfn\\s+main\\s*\\(",
     "rustc --crate-type=lib --emit=metadata -A dead_code -o \"$1/check\" \"$1/cell.rs\""},
    {"java", "\\bvoid\\s+main\\s*\\(", "javac -d \"$1\" \"$1/Main.java\""},
};

/// Whether `source` has an entry point for `tool`, which makes it a program
/// rather than a snippet. Comments are stripped first so a mention of main in
/// one does not count. A tool this knows no entry point for (python...) says
/// no: a script has none to find.
bool hasEntry(const QString &tool, QString source)
{
    // Haskell: with a `main` it is a program, without a ghci session.
    if (tool == QLatin1String("ghci")) {
        static const QRegularExpression main(QStringLiteral("^main\\s*(::|=)"), QRegularExpression::MultilineOption);
        return main.match(source).hasMatch();
    }
    static const QRegularExpression comments(QStringLiteral("//[^\\n]*|/\\*.*?\\*/"),
                                             QRegularExpression::DotMatchesEverythingOption);
    source.remove(comments);
    for (const Check &check : checks) {
        if (tool == QLatin1String(check.tool))
            return QRegularExpression(QString::fromLatin1(check.entry)).match(source).hasMatch();
    }
    return false;
}

/// The shell script that only type-checks a block with no entry point (a
/// snippet in a note, not a program), `$1` the block's directory and `$2` the
/// line to print when it is fine; empty when the language has no such mode or
/// the block has a `main`.
QString checkScript(const QString &tool, const QString &source)
{
    for (const Check &check : checks) {
        if (tool == QLatin1String(check.tool) && !hasEntry(tool, source))
            return QString::fromLatin1(check.command) + QStringLiteral(" && printf '%s\\n' \"$2\"");
    }
    return {};
}

/// A compiler or interpreter names the line in the file it was given, which
/// has the note's implicit code (and a wrapper's opening) above the block:
/// give the block's own line instead, and say "implicit code" for a line of
/// the prelude. Only the forms the file names appear in (`cell.c:3:5`,
/// `Main.java:3`, `cell.py", line 3`) are touched.
QString blockLines(const QString &text, int offset)
{
    static const QRegularExpression named(
        QStringLiteral("\"?[^\\s\"]*(cell\\.[a-z]+|Main\\.java)(:|\", line )(\\d+)"));
    QString out;
    qsizetype from = 0;
    for (auto it = named.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const int line = match.captured(3).toInt();
        out += QStringView(text).mid(from, match.capturedStart() - from);
        if (line > offset)
            out += QStringView(text).mid(match.capturedStart(), match.capturedStart(3) - match.capturedStart())
                   + QString::number(line - offset);
        else
            out += QStringLiteral("implicit code, line %1").arg(line);
        from = match.capturedEnd();
    }
    return out + QStringView(text).mid(from);
}

QString withoutAnsi(QString text)
{
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    return text.remove(ansi);
}

constexpr qint64 kMaxOutput = 1 << 20; // a runaway print loop stops here
/// A block's own `#| timeout:` is honoured up to here, and its limits follow: a cell is user-started code of
/// a note anyone may have written.
constexpr int kMaxTimeoutMs = 300'000;
constexpr rlim_t kMemoryBytes = 4ULL * 1024 * 1024 * 1024; // RLIMIT_DATA: what is committed, not what a JIT reserves
constexpr rlim_t kFileBytes = 256ULL * 1024 * 1024;       // the biggest file a cell writes (a compiled binary, a log)
constexpr qint64 kWasmMemory = qint64(1) << 30; // a guest's linear memory, 1 GiB

/// `path` with its symlinks resolved, bound into the sandbox unless it is
/// under /usr (already bound). The caller runs the returned path: a symlink's
/// own location does not exist inside the sandbox.
QString bindIfOutsideUsr(QStringList *binds, const QString &path)
{
    const QString real = QFileInfo(path).canonicalFilePath();
    if (real.isEmpty())
        return path;
    if (!real.startsWith(QLatin1String("/usr/")) && !binds->contains(real))
        binds->append(real);
    return real;
}

} // namespace

struct DirectRunner::Running {
    QString language;
    std::unique_ptr<QTemporaryDir> dir;
    QProcess *process = nullptr;
    QTimer timeout;
    QElapsedTimer clock;
    Deliver deliver;
    bool timedOut = false;
    bool interrupted = false;
    int blockLineOffset = 0; ///< lines above the block in the file that ran
};

DirectRunner::DirectRunner(QObject *parent)
    : QObject(parent)
{
}

DirectRunner::~DirectRunner()
{
    for (Running *running : std::as_const(m_running)) {
        running->process->disconnect();
        running->process->kill();
        running->process->waitForFinished(1000);
        delete running;
    }
}

QString DirectRunner::toolFor(const QString &language)
{
    const Recipe *recipe = recipeFor(language);
    return recipe ? recipe->tool : QString();
}

bool DirectRunner::canRun(const QString &language)
{
    const QString tool = toolFor(language);
    return !tool.isEmpty() && !QStandardPaths::findExecutable(tool).isEmpty();
}

QString DirectRunner::wasmModuleFor(const QString &setting, const QString &language)
{
    const QString wanted = language.trimmed().toLower();
    const Recipe *recipe = recipeFor(wanted);
    for (const QString &entry : setting.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const qsizetype eq = entry.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = entry.left(eq).trimmed().toLower();
        if (key == wanted || (recipe && recipeFor(key) == recipe))
            return entry.mid(eq + 1).trimmed();
    }
    return {};
}

QString DirectRunner::composed(const Request &request, int *blockLineOffset)
{
    if (blockLineOffset)
        *blockLineOffset = 0;
    if (request.prelude.isEmpty() && request.wrapper.isEmpty())
        return request.source;
    QString text = request.source;
    int offset = 0;
    // A wrapper is for a snippet: a block with its own entry point keeps it.
    const qsizetype slot = request.wrapper.indexOf(QLatin1String("{{code}}"));
    if (slot >= 0 && !hasEntry(toolFor(request.language), request.source)) {
        offset = int(QStringView(request.wrapper).left(slot).count(QLatin1Char('\n')));
        text = QString(request.wrapper).replace(QLatin1String("{{code}}"), request.source);
    }
    if (!request.prelude.isEmpty()) {
        offset += int(request.prelude.count(QLatin1Char('\n'))) + 1;
        text = request.prelude + QLatin1Char('\n') + text;
    }
    if (blockLineOffset)
        *blockLineOffset = offset;
    return text;
}

void DirectRunner::run(const Request &request, const Deliver &deliver)
{
    const auto fail = [this, deliver](const QString &error) {
        NotebookRunResult result;
        result.hasRun = true;
        result.ok = false;
        result.kernelError = error;
        QTimer::singleShot(0, this, [deliver, result] { deliver(result); });
    };
    const Recipe *recipe = recipeFor(request.language);
    if (!recipe)
        return fail(tr("Notolith does not know how to run \"%1\"").arg(request.language));
    // The WASM tier runs a module under wasmtime, not the language's own
    // tool, so that tool need not be installed - wasmtime must be.
    const bool wasm = request.tier == SandboxTier::Wasm;
    const bool isModule = recipe->tool == QLatin1String("wasmtime"); // a wat/wasm block
    const QString wasmtime = QStandardPaths::findExecutable(QStringLiteral("wasmtime"));
    if (wasm && wasmtime.isEmpty())
        return fail(tr("wasmtime is needed for the wasm sandbox, and it is not installed"));
    if (wasm && !isModule) {
        if (request.wasmModule.isEmpty())
            return fail(tr("no WebAssembly module set for \"%1\" blocks (Settings, notebooks.wasmModules)")
                            .arg(request.language));
        if (!QFileInfo(request.wasmModule).isFile())
            return fail(tr("the WebAssembly module for \"%1\" blocks, %2, is not a file")
                            .arg(request.language, request.wasmModule));
    }
    if (!wasm && !canRun(request.language))
        return fail(tr("%1 is not installed, so \"%2\" blocks cannot run").arg(recipe->tool, request.language));

    // The block as the tool will see it, the note's implicit code included:
    // that is what has a `main` or not.
    int blockLineOffset = 0;
    const QString composedSource = composed(request, &blockLineOffset);
    // Haskell below, and these: a block with no `main` is only checked.
    const QString checker = wasm ? QString() : checkScript(recipe->tool, composedSource);
    if (!checker.isEmpty() && recipe->tool == QLatin1String("java")
        && QStandardPaths::findExecutable(QStringLiteral("javac")).isEmpty())
        return fail(tr("javac is needed to check a java block with no main, and it is not installed"));

    auto *running = new Running;
    running->language = request.language.trimmed().toLower();
    running->deliver = deliver;
    running->blockLineOffset = blockLineOffset;
    running->dir = std::make_unique<QTemporaryDir>();
    if (!running->dir->isValid()) {
        delete running;
        return fail(tr("no temporary directory to run in"));
    }
    const QString dir = running->dir->path();

    // Haskell with a `main` is a program; without, a ghci session.
    Recipe chosen = *recipe;
    QString text = composedSource;
    if (chosen.tool == QLatin1String("ghci") && !wasm) { // a wasm module gets the block as written
        if (hasEntry(chosen.tool, text)) {
            chosen.file = QStringLiteral("cell.hs");
            chosen.command = {QStringLiteral("runghc"), QStringLiteral("%f")};
        } else {
            text = ghciScript(text);
        }
    }
    if (!checker.isEmpty()) {
        chosen.command = {QStringLiteral("sh"), QStringLiteral("-c"), checker, QStringLiteral("sh"), QStringLiteral("%d"),
                          tr("Checked, no errors. Nothing to run: this block has no main.")};
    }
    const QString file = dir + QLatin1Char('/') + chosen.file;
    {
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly)) {
            delete running;
            return fail(tr("could not write the block to run it"));
        }
        out.write(text.toUtf8());
    }

    QStringList command;
    for (const QString &part : std::as_const(chosen.command)) {
        QString arg = part;
        arg.replace(QStringLiteral("%f"), file);
        arg.replace(QStringLiteral("%d"), dir);
        command << arg;
    }
    SandboxRequest sandbox;
    sandbox.runtimeDir = dir;
    sandbox.readOnlyBinds = request.readOnlyBinds;
    // 1 GiB of RAM for the cell, its files and /dev/shm together (the default); a wasm guest's linear memory
    // alone may be that big, so its runtime gets room on top.
    if (wasm)
        sandbox.memoryBytes = kWasmMemory + (256LL << 20);
    QString program;
    QStringList args;
    SandboxTier used = SandboxTier::Bubblewrap;
    int filterFD = -1;
    if (wasm) {
        // The guest sees the block's directory as /cell and nothing else, and
        // memory is capped. `request.readOnlyBinds` (a runnable embed's
        // folder) is deliberately not granted: wasmtime 49 has no read-only
        // --dir, so the guest could write into the user's vault.
        // Then the whole of wasmtime inside tier 1's bubblewrap + seccomp, so
        // a wasmtime bug is still contained; only the module and the runtime
        // (when they are outside /usr) are bound in, as single files.
        sandbox.readOnlyBinds.clear();
        const QString runtime = bindIfOutsideUsr(&sandbox.readOnlyBinds, wasmtime);
        // A wat/wasm block's module is in the cell directory, bound already.
        const QString module = isModule ? file : bindIfOutsideUsr(&sandbox.readOnlyBinds, request.wasmModule);
        // `--` so a module path starting with '-' is not read as an option.
        QStringList wasmArgs = {QStringLiteral("run"), QStringLiteral("-C"), QStringLiteral("cache=n"),
                                QStringLiteral("--dir"), dir + QStringLiteral("::/cell"),
                                QStringLiteral("-W"), QStringLiteral("max-memory-size=%1").arg(kWasmMemory),
                                QStringLiteral("--"), module};
        if (!isModule)
            wasmArgs << QStringLiteral("/cell/") + chosen.file;
        wrapCommand(SandboxTier::Bubblewrap, runtime, wasmArgs, sandbox, &program, &args, &used, &filterFD);
        // No bubblewrap, no run, as in tier 1: WASI alone would leave a
        // wasmtime bug with the user's files and network, and no seccomp.
        if (program.isEmpty()) {
            delete running;
            return fail(tr("code runs only inside bubblewrap's sandbox (with libseccomp); install bubblewrap"));
        }
        used = SandboxTier::Wasm;
    } else {
        wrapCommand(request.tier, command.takeFirst(), command, sandbox, &program, &args, &used,
                    &filterFD);
        if (program.isEmpty()) {
            delete running;
            return fail(tr("code runs only inside bubblewrap's sandbox (with libseccomp); install bubblewrap"));
        }
    }

    running->process = new QProcess(this);
    running->process->setProgram(program);
    running->process->setArguments(args);
    running->process->setWorkingDirectory(dir);
    // bwrap gets none of the app's environment (the sandbox sets its own PATH and HOME): nothing of the
    // session's tokens, and nothing of the real home - caches and histories land in the throwaway directory.
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    running->process->setProcessEnvironment(env);
    running->process->setStandardInputFile(QProcess::nullDevice());
    const int timeoutMs = request.timeoutMs > 0 ? qMin(request.timeoutMs, kMaxTimeoutMs) : kMaxTimeoutMs;
    sandbox::Limits limits;
    limits.data = kMemoryBytes;
    limits.fileSize = kFileBytes;
    limits.cpuSeconds = rlim_t(timeoutMs / 1000 + 5);
    limits.openFiles = 1024;
    running->process->setChildProcessModifier([filterFD, limits] { sandbox::prepareChild(limits, filterFD); });

    // Output beyond the cap stops the run, on either channel.
    const auto capped = [running] {
        QProcess *process = running->process;
        const QProcess::ProcessChannel was = process->readChannel();
        process->setReadChannel(QProcess::StandardOutput);
        bool tooMuch = process->bytesAvailable() > kMaxOutput;
        process->setReadChannel(QProcess::StandardError);
        tooMuch = tooMuch || process->bytesAvailable() > kMaxOutput;
        process->setReadChannel(was);
        if (tooMuch)
            process->kill();
    };
    connect(running->process, &QProcess::readyReadStandardOutput, this, capped);
    connect(running->process, &QProcess::readyReadStandardError, this, capped);
    connect(running->process, &QProcess::finished, this, [this, running](int code, QProcess::ExitStatus status) {
        NotebookRunResult result;
        result.hasRun = true;
        result.wallMs = running->clock.elapsed();
        const QString out = withoutAnsi(QString::fromUtf8(running->process->readAllStandardOutput().left(kMaxOutput)));
        QString err = withoutAnsi(QString::fromUtf8(running->process->readAllStandardError().left(kMaxOutput)));
        if (running->blockLineOffset > 0)
            err = blockLines(err, running->blockLineOffset);
        if (!out.isEmpty())
            result.outputs.append({NotebookOutput::Kind::Text, out, {}, {}, {}, {}});
        // ghci reports a failed expression on stderr and still exits 0.
        const bool ghciFailed = toolFor(running->language) == QLatin1String("ghci")
                                && err.contains(QLatin1String("error"));
        result.ok = status == QProcess::NormalExit && code == 0 && !running->timedOut && !ghciFailed;
        if (!result.ok || !err.trimmed().isEmpty()) {
            NotebookOutput error;
            error.kind = result.ok ? NotebookOutput::Kind::Text : NotebookOutput::Kind::Error;
            error.isStderr = result.ok; // a warning on a run that worked: drawn as stderr
            error.ename = running->timedOut      ? tr("Timed out")
                          : running->interrupted ? tr("Interrupted")
                                                 : tr("Exit code %1").arg(code);
            error.evalue = running->timedOut ? tr("stopped after %1 s").arg(running->timeout.interval() / 1000)
                                             : QString();
            // "ZeroDivisionError: division by zero", the way a traceback ends,
            // names it better than an exit code does.
            static const QRegularExpression named(QStringLiteral("^(\\w+(?:Error|Exception)): ?(.*)$"),
                                                  QRegularExpression::MultilineOption);
            QRegularExpressionMatch last;
            for (auto it = named.globalMatch(err); it.hasNext();)
                last = it.next();
            if (!running->timedOut && !running->interrupted && last.hasMatch()) {
                error.ename = last.captured(1);
                error.evalue = last.captured(2);
            }
            error.text = err;
            error.traceback = err.split(QLatin1Char('\n'));
            result.outputs.append(error);
        }
        m_running.removeOne(running);
        const Deliver deliver = running->deliver;
        running->process->deleteLater();
        delete running;
        deliver(result);
    });
    running->timeout.setSingleShot(true);
    running->timeout.setInterval(timeoutMs);
    connect(&running->timeout, &QTimer::timeout, this, [running] {
        running->timedOut = true;
        running->process->kill();
    });

    m_running.append(running);
    running->clock.start();
    running->timeout.start();
    running->process->start();
    // The fork has happened; bwrap holds its own copy of the filter.
    if (filterFD >= 0)
        close(filterFD);
}

void DirectRunner::interrupt(const QString &language)
{
    const QString wanted = language.trimmed().toLower();
    for (Running *running : std::as_const(m_running)) {
        if (wanted.isEmpty() || running->language == wanted) {
            running->interrupted = true;
            running->process->kill();
        }
    }
}

} // namespace noteAppC::notebook
