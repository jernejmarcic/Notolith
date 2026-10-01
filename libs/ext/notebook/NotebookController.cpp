#include "NotebookController.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace noteAppC::notebook {

namespace {

/// A traceback line arrives with ANSI colour escapes (real Python
/// tracebacks are colourised by IPython); stripped rather than mapped to
/// Qt colours - see NotebookOutput's own comment for the upgrade path if
/// coloured tracebacks turn out to matter.
QString stripAnsi(const QString &text)
{
    static const QRegularExpression escape(QStringLiteral("\x1b\\[[0-9;]*m"));
    QString out = text;
    out.remove(escape);
    return out;
}

/// A carriage return rewrites the line it is on (tqdm, a progress counter):
/// only what follows the last one on a line stays. ponytail: whole-line
/// rewrites, not a terminal's overwrite-in-place, which is all those print.
QString resolveCarriageReturns(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (!text.contains(QLatin1Char('\r')))
        return text;
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        const qsizetype cr = line.lastIndexOf(QLatin1Char('\r'));
        if (cr >= 0)
            line = line.mid(cr + 1);
    }
    return lines.join(QLatin1Char('\n'));
}

/// What a notebook shows of one display bundle, richest first, as Jupyter
/// frontends choose: a picture, then HTML, then LaTeX and Markdown, then the
/// text every kernel sends. Nothing recognised: an empty Text output, which
/// the caller drops.
NotebookOutput outputFromMimeBundle(const QJsonObject &data)
{
    NotebookOutput output;
    const auto text = [&data](const char *mime) { return data.value(QLatin1String(mime)).toString(); };
    if (data.contains(QLatin1String("image/png"))) {
        output.kind = NotebookOutput::Kind::ImagePng;
        output.imageBytes = QByteArray::fromBase64(text("image/png").toLatin1());
    } else if (data.contains(QLatin1String("image/jpeg"))) {
        output.kind = NotebookOutput::Kind::ImagePng; // the editor reads either
        output.imageBytes = QByteArray::fromBase64(text("image/jpeg").toLatin1());
    } else if (data.contains(QLatin1String("image/svg+xml"))) {
        output.kind = NotebookOutput::Kind::ImageSvg;
        output.text = text("image/svg+xml");
    } else if (data.contains(QLatin1String("text/html"))) {
        output.kind = NotebookOutput::Kind::Html;
        output.text = text("text/html");
        output.alt = stripAnsi(text("text/plain"));
    } else if (data.contains(QLatin1String("text/latex"))) {
        // Through the maths renderer the Markdown path already has.
        output.kind = NotebookOutput::Kind::Markdown;
        const QString tex = text("text/latex").trimmed();
        output.text = tex.startsWith(QLatin1Char('$')) ? tex : QStringLiteral("$$\n") + tex + QStringLiteral("\n$$");
    } else if (data.contains(QLatin1String("text/markdown"))) {
        output.kind = NotebookOutput::Kind::Markdown;
        output.text = text("text/markdown");
    } else if (data.contains(QLatin1String("text/plain"))) {
        output.kind = NotebookOutput::Kind::Text;
        output.text = stripAnsi(text("text/plain"));
    }
    return output;
}

bool hasContent(const NotebookOutput &output)
{
    return !output.text.isEmpty() || !output.imageBytes.isEmpty();
}

/// A runaway print loop or a plot in a loop must not fill memory.
constexpr qint64 kMaxTextChars = 1 << 20;
constexpr qint64 kMaxImageBytes = 16 << 20;

bool overCap(const NotebookRunResult &result)
{
    qint64 chars = 0;
    qint64 bytes = 0;
    for (const NotebookOutput &output : result.outputs) {
        chars += output.text.size();
        bytes += output.imageBytes.size();
    }
    return chars > kMaxTextChars || bytes > kMaxImageBytes;
}

/// The protocol counts the caret and the replaced range in Unicode code
/// points, QString in UTF-16 units: the two differ after an emoji.
int codePointsBefore(const QString &text, qsizetype units)
{
    int count = 0;
    for (qsizetype i = 0; i < units && i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            ++i;
        ++count;
    }
    return count;
}

qsizetype unitsOfCodePoints(const QString &text, int codePoints)
{
    qsizetype i = 0;
    for (int n = 0; n < codePoints && i < text.size(); ++n) {
        i += text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate() ? 2 : 1;
    }
    return i;
}

} // namespace

NotebookController::NotebookController(QObject *parent) : QObject(parent) { }

NotebookController::~NotebookController()
{
    shutdownAll();
}

void NotebookController::run(const RunRequest &request, bool forceRun, const Deliver &deliver)
{
    const QString cacheKey = notebookCacheKey(request.language, request.source, request.tier);

    if (!forceRun) {
        // Cache-only: never starts a kernel. Deferred one tick, like every
        // other answer in this app that could technically be synchronous
        // (layoutNoteEmbed's own QTimer::singleShot(0, ...) is the same
        // discipline) so a caller never has to special-case "answered
        // inline" against "answered later". `#| cache: false` keeps nothing,
        // so there is nothing to find.
        const bool cached = request.cache;
        QTimer::singleShot(0, this, [cacheKey, deliver, cached] {
            deliver(cached ? readNotebookCache(cacheKey) : NotebookRunResult());
        });
        return;
    }

    // The system's own interpreter or compiler first: one short process, no
    // kernel to install or keep running. A Jupyter kernel for a language
    // noteAppC does not know how to run itself, or when the block (or the
    // setting) asks for one so that its blocks share variables.
    const QString tool = DirectRunner::toolFor(request.language);
    const bool wantsKernel = request.useKernel || !request.kernelName.isEmpty();
    if (!wantsKernel
        && (!tool.isEmpty() || !kernelSpecsByLanguage().contains(request.language.trimmed().toLower()))) {
        if (!m_direct)
            m_direct = new DirectRunner(this);
        DirectRunner::Request direct;
        direct.language = request.language;
        direct.source = request.source;
        direct.tier = request.tier;
        direct.readOnlyBinds = request.extraReadOnlyBinds;
        if (request.timeoutMs > 0)
            direct.timeoutMs = request.timeoutMs;
        const bool keep = request.cache;
        m_direct->run(direct, [cacheKey, deliver, keep](NotebookRunResult result) {
            if (keep && result.kernelError.isEmpty())
                writeNotebookCache(cacheKey, result);
            deliver(result);
        });
        return;
    }

    QString error;
    KernelSession *session = ensureKernel(request, &error);
    if (!session) {
        NotebookRunResult result;
        result.kernelError = error;
        QTimer::singleShot(0, this, [result, deliver] { deliver(result); });
        return;
    }

    const QString msgID = session->client->execute(request.source);
    InFlight inFlight;
    inFlight.deliver = deliver;
    inFlight.clock.start();
    inFlight.cacheKey = cacheKey;
    inFlight.language = request.language;
    inFlight.cache = request.cache;
    inFlight.timeoutMs = request.timeoutMs;
    session->inFlight.insert(msgID, inFlight);
    if (request.timeoutMs > 0) {
        // Looked up again when it fires: the kernel may have been restarted
        // and the block long since answered.
        QTimer::singleShot(request.timeoutMs, this, [this, key = session->key, msgID] {
            const auto kernel = m_kernels.find(key);
            if (kernel == m_kernels.end())
                return;
            const auto pending = kernel->second->inFlight.find(msgID);
            if (pending == kernel->second->inFlight.end())
                return;
            pending->timedOut = true;
            interruptSession(kernel->second.get());
        });
    }
}

NotebookController::KernelSession *NotebookController::ensureKernel(const RunRequest &request,
                                                                     QString *error)
{
    const QString language = request.language.trimmed().toLower();
    KernelSpec spec;
    if (!request.kernelName.isEmpty()) {
        bool found = false;
        for (const KernelSpec &candidate : discoverKernelSpecs()) {
            if (candidate.name.compare(request.kernelName, Qt::CaseInsensitive) == 0) {
                spec = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            *error = QStringLiteral("no Jupyter kernel named \"%1\" is installed").arg(request.kernelName);
            return nullptr;
        }
    } else {
        const QHash<QString, KernelSpec> specs = kernelSpecsByLanguage();
        const auto specIt = specs.constFind(language);
        if (specIt == specs.constEnd()) {
            *error = QStringLiteral("no Jupyter kernel installed for language \"%1\"").arg(request.language);
            return nullptr;
        }
        spec = specIt.value();
    }

    // The tier is part of the key: a block that asked for the host must never
    // end up in a sandboxed kernel's state, nor a sandboxed one in a host
    // kernel that can read the home directory.
    const QString key = request.scope + QLatin1Char('\x1f') + QString::number(int(request.tier))
                        + QLatin1Char('\x1f') + spec.name;
    auto it = m_kernels.find(key);
    if (it != m_kernels.end()) {
        if (it->second->process->isRunning())
            return it->second.get();
        // It died (the code called exit(), the OOM killer...): start another
        // rather than keep answering from a corpse.
        failInFlight(it->second.get(), QStringLiteral("the kernel exited"));
        m_kernels.erase(it);
    }

    auto session = std::make_unique<KernelSession>();
    session->key = key;
    session->scope = request.scope;
    session->language = spec.language.toLower();
    session->spec = spec;
    session->tierRequested = request.tier;
    session->process = std::make_unique<KernelProcess>();
    if (!session->process->start(session->spec, request.tier, request.extraReadOnlyBinds)) {
        *error = session->process->error();
        return nullptr;
    }

    session->client = std::make_unique<KernelClient>();
    KernelSession *raw = session.get();
    wireKernel(raw);
    // A kernel that exits under a running block would otherwise leave it
    // "Running..." for good.
    connect(session->process.get(), &KernelProcess::finished, this, [this, key](int code) {
        const auto kernel = m_kernels.find(key);
        if (kernel == m_kernels.end())
            return;
        failInFlight(kernel->second.get(), QStringLiteral("the kernel exited (code %1)").arg(code));
        emit kernelStateChanged(kernel->second->language, false);
    });
    session->client->startClient(session->process->connectionInfo(),
                                 QUuid::createUuid().toString(QUuid::WithoutBraces),
                                 QStringLiteral("noteAppC"));

    m_kernels.emplace(key, std::move(session));
    emit kernelStateChanged(language, true);
    return raw;
}

void NotebookController::wireKernel(KernelSession *session)
{
    KernelClient *client = session->client.get();

    // Everything that adds to a run goes through here, so the cap is one rule.
    const auto add = [](InFlight &run, NotebookOutput output) {
        if (run.truncated)
            return;
        run.result.outputs.append(std::move(output));
        if (overCap(run.result)) {
            run.truncated = true;
            NotebookOutput note;
            note.kind = NotebookOutput::Kind::Text;
            note.isStderr = true;
            note.text = QStringLiteral("[output truncated]");
            run.result.outputs.append(note);
        }
    };

    connect(client, &KernelClient::streamOutput, this,
           [session, add](const QString &parent, const QString &name, const QString &text) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end() || it->truncated)
                   return;
               // In the order the kernel said it: a stream continues the
               // output before it when that is the same stream, and starts a
               // new one after a picture or a result.
               const bool isStderr = name == QLatin1String("stderr");
               QList<NotebookOutput> &outputs = it->result.outputs;
               if (!outputs.isEmpty() && outputs.last().kind == NotebookOutput::Kind::Text
                   && outputs.last().displayID.isEmpty() && outputs.last().isStderr == isStderr) {
                   outputs.last().text = resolveCarriageReturns(outputs.last().text + stripAnsi(text));
                   if (overCap(it->result)) {
                       it->truncated = true;
                       outputs.last().text.truncate(kMaxTextChars);
                   }
                   return;
               }
               NotebookOutput output;
               output.kind = NotebookOutput::Kind::Text;
               output.isStderr = isStderr;
               output.text = resolveCarriageReturns(stripAnsi(text));
               add(*it, output);
           });
    connect(client, &KernelClient::executeResultReceived, this,
           [session, add](const QString &parent, const QJsonObject &data, int) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               NotebookOutput output = outputFromMimeBundle(data);
               if (hasContent(output))
                   add(*it, output);
           });
    connect(client, &KernelClient::displayDataReceived, this,
           [session, add](const QString &parent, const QJsonObject &data, const QString &displayID,
                          bool isUpdate) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               NotebookOutput output = outputFromMimeBundle(data);
               if (!hasContent(output))
                   return;
               output.displayID = displayID;
               // update_display_data redraws an earlier output in place (a
               // progress bar); with nothing to replace it is just another.
               if (isUpdate && !displayID.isEmpty()) {
                   for (NotebookOutput &earlier : it->result.outputs) {
                       if (earlier.displayID == displayID) {
                           earlier = output;
                           return;
                       }
                   }
               }
               add(*it, output);
           });
    connect(client, &KernelClient::clearOutputReceived, this, [session](const QString &parent) {
        // `wait` (clear when the next output arrives) would only change when
        // the screen flickers, and nothing is drawn before the run ends.
        auto it = session->inFlight.find(parent);
        if (it != session->inFlight.end())
            it->result.outputs.clear();
    });
    connect(client, &KernelClient::errorReceived, this,
           [session, add](const QString &parent, const QString &ename, const QString &evalue,
                          const QStringList &traceback) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               NotebookOutput output;
               output.kind = NotebookOutput::Kind::Error;
               output.ename = ename;
               output.evalue = evalue;
               for (const QString &line : traceback)
                   output.traceback << stripAnsi(line);
               it->result.ok = false;
               add(*it, output);
           });
    connect(client, &KernelClient::executeReplyReceived, this,
           [this, session](const QString &parent, bool ok, int count) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               it->gotReply = true;
               it->replyOk = ok;
               it->executionCount = qMax(0, count);
               finalizeIfDone(session, it);
           });
    connect(client, &KernelClient::introspectionReplyReceived, this,
           [session](const QString &parent, const QString &type, const QJsonObject &content) {
               const auto it = session->introspections.find(parent);
               if (it == session->introspections.end())
                   return; // cancelled, or timed out: the answer is not wanted any more
               const KernelSession::Pending pending = it.value();
               session->introspections.erase(it);
               Introspection answer;
               answer.ok = content.value(QStringLiteral("status")).toString() == QLatin1String("ok");
               if (type == QLatin1String("complete_reply")) {
                   for (const QJsonValue &match : content.value(QStringLiteral("matches")).toArray())
                       answer.matches << match.toString();
                   answer.from = unitsOfCodePoints(pending.code, content.value(QStringLiteral("cursor_start")).toInt());
                   answer.to = unitsOfCodePoints(pending.code, content.value(QStringLiteral("cursor_end")).toInt());
               } else if (content.value(QStringLiteral("found")).toBool()) {
                   answer.text = stripAnsi(content.value(QStringLiteral("data")).toObject()
                                               .value(QStringLiteral("text/plain")).toString());
               }
               pending.deliver(answer);
           });
    connect(client, &KernelClient::statusChanged, this,
           [this, session](const QString &parent, const QString &state) {
               if (state != QLatin1String("idle"))
                   return; // "busy" carries nothing this needs
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               it->gotIdle = true;
               finalizeIfDone(session, it);
           });
}

void NotebookController::finalizeIfDone(KernelSession *session, QHash<QString, InFlight>::iterator it)
{
    // Shell (execute_reply) and iopub (status: idle) are independent
    // sockets - either can arrive first - so a request is only really over
    // once both have: idle is the kernel's own promise that every output
    // message it will ever send for this request is already on the wire
    // ahead of it (iopub preserves send order for one sender), and the
    // reply carries the ok/error status finalize needs regardless.
    if (!it->gotReply || !it->gotIdle)
        return;
    NotebookRunResult result = it->result;
    result.hasRun = true;
    result.ok = result.ok && it->replyOk;
    result.wallMs = it->clock.elapsed();
    result.executionCount = it->executionCount;
    if (it->timedOut) {
        // The KeyboardInterrupt the stop caused says less than why it was sent.
        result.outputs.erase(std::remove_if(result.outputs.begin(), result.outputs.end(),
                                            [](const NotebookOutput &output) {
                                                return output.kind == NotebookOutput::Kind::Error;
                                            }),
                             result.outputs.end());
        NotebookOutput timeout;
        timeout.kind = NotebookOutput::Kind::Error;
        timeout.ename = QStringLiteral("Timed out");
        timeout.evalue = QStringLiteral("stopped after %1 s").arg(it->timeoutMs / 1000);
        result.outputs.append(timeout);
        result.ok = false;
    }
    if (it->cache)
        writeNotebookCache(it->cacheKey, result);
    const Deliver deliver = it->deliver;
    session->inFlight.erase(it);
    deliver(result);
}

void NotebookController::failInFlight(KernelSession *session, const QString &error)
{
    const auto asked = std::exchange(session->introspections, {});
    for (const auto &entry : asked)
        entry.deliver(Introspection());
    const QHash<QString, InFlight> pending = session->inFlight;
    session->inFlight.clear();
    for (const InFlight &entry : pending) {
        NotebookRunResult result;
        result.kernelError = error;
        entry.deliver(result);
    }
}

NotebookController::KernelSession *NotebookController::runningKernelFor(const RunRequest &request)
{
    const QString language = request.language.trimmed().toLower();
    for (auto &entry : m_kernels) {
        KernelSession *session = entry.second.get();
        const bool named = request.kernelName.isEmpty() ? session->language == language
                                                        : session->spec.name.compare(request.kernelName, Qt::CaseInsensitive) == 0;
        if (named && session->scope == request.scope && session->tierRequested == request.tier
            && session->process->isRunning())
            return session;
    }
    return nullptr;
}

bool NotebookController::introspect(const RunRequest &request, const QString &code, qsizetype cursor,
                                    bool inspect, const DeliverIntrospection &deliver)
{
    KernelSession *session = runningKernelFor(request);
    if (!session)
        return false;
    // One question of each kind at a time: a newer one makes the older stale.
    for (auto it = session->introspections.begin(); it != session->introspections.end();) {
        if (it->inspect != inspect) {
            ++it;
            continue;
        }
        const DeliverIntrospection stale = it->deliver;
        it = session->introspections.erase(it);
        stale(Introspection());
    }
    const int point = codePointsBefore(code, cursor);
    const QString msgID = inspect ? session->client->inspect(code, point) : session->client->complete(code, point);
    session->introspections.insert(msgID, {deliver, inspect, code});
    // A kernel busy with a long cell answers only when it is done: not worth waiting for.
    QTimer::singleShot(4000, this, [this, key = session->key, msgID] {
        const auto kernel = m_kernels.find(key);
        if (kernel == m_kernels.end())
            return;
        const auto it = kernel->second->introspections.find(msgID);
        if (it == kernel->second->introspections.end())
            return;
        const DeliverIntrospection late = it->deliver;
        kernel->second->introspections.erase(it);
        late(Introspection());
    });
    return true;
}

void NotebookController::interruptSession(KernelSession *session)
{
    // Exactly one, chosen by what the kernelspec actually declares - not
    // both. The installed python3 kernelspec sets no interrupt_mode, so
    // KernelSpec defaults it to "signal"; sending interrupt_request on the
    // control channel as well, "just in case", turned out not to be
    // harmless (caught by the end-to-end test, not guessed at): ipykernel
    // answers it, but the original cell's own execute_reply on the shell
    // channel then never arrives, wedging finalizeIfDone forever.
    if (session->spec.interruptMode == QLatin1String("message"))
        session->client->requestInterrupt();
    else
        session->process->interrupt();
}

void NotebookController::interruptKernel(const QString &language, const QString &scope)
{
    if (m_direct)
        m_direct->interrupt(language);
    const QString wanted = language.trimmed().toLower();
    for (auto &entry : m_kernels) {
        KernelSession *session = entry.second.get();
        if ((scope.isEmpty() || session->scope == scope)
            && (wanted.isEmpty() || session->language == wanted))
            interruptSession(session);
    }
}

void NotebookController::restartKernel(const QString &language, const QString &scope)
{
    const QString wanted = language.trimmed().toLower();
    for (auto it = m_kernels.begin(); it != m_kernels.end();) {
        KernelSession *session = it->second.get();
        if ((!scope.isEmpty() && session->scope != scope)
            || (!wanted.isEmpty() && session->language != wanted)) {
            ++it;
            continue;
        }
        failInFlight(session, QStringLiteral("kernel restarted"));
        const QString stopped = session->language;
        it = m_kernels.erase(it); // ~KernelSession stops the client thread and kills the process
        emit kernelStateChanged(stopped, false);
    }
}

void NotebookController::shutdownAll()
{
    for (auto &entry : m_kernels)
        failInFlight(entry.second.get(), QStringLiteral("kernel shut down"));
    m_kernels.clear();
}

bool NotebookController::hasRunningKernel(const QString &language) const
{
    const QString wanted = language.trimmed().toLower();
    for (const auto &entry : m_kernels) {
        if (entry.second->language == wanted && entry.second->process->isRunning())
            return true;
    }
    return false;
}

int NotebookController::runningKernelCount(const QString &scope) const
{
    int count = 0;
    for (const auto &entry : m_kernels)
        count += (scope.isEmpty() || entry.second->scope == scope) && entry.second->process->isRunning() ? 1 : 0;
    return count;
}

} // namespace noteAppC::notebook
