#include "NotebookController.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

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

NotebookOutput outputFromMimeBundle(const QJsonObject &data)
{
    NotebookOutput output;
    if (data.contains(QLatin1String("image/png"))) {
        output.kind = NotebookOutput::Kind::ImagePng;
        output.imageBytes =
            QByteArray::fromBase64(data.value(QStringLiteral("image/png")).toString().toLatin1());
    } else if (data.contains(QLatin1String("image/svg+xml"))) {
        output.kind = NotebookOutput::Kind::ImageSvg;
        output.text = data.value(QStringLiteral("image/svg+xml")).toString();
    } else if (data.contains(QLatin1String("text/markdown"))) {
        output.kind = NotebookOutput::Kind::Markdown;
        output.text = data.value(QStringLiteral("text/markdown")).toString();
    } else if (data.contains(QLatin1String("text/plain"))) {
        output.kind = NotebookOutput::Kind::Text;
        output.text = stripAnsi(data.value(QStringLiteral("text/plain")).toString());
    }
    return output;
}

} // namespace

NotebookController::NotebookController(QObject *parent) : QObject(parent) { }

NotebookController::~NotebookController()
{
    shutdownAll();
}

void NotebookController::run(const RunRequest &request, bool forceRun, const Deliver &deliver)
{
    const QString cacheKey = notebookCacheKey(request.language, request.source);

    if (!forceRun) {
        // Cache-only: never starts a kernel. Deferred one tick, like every
        // other answer in this app that could technically be synchronous
        // (layoutNoteEmbed's own QTimer::singleShot(0, ...) is the same
        // discipline) so a caller never has to special-case "answered
        // inline" against "answered later".
        QTimer::singleShot(0, this, [cacheKey, deliver] { deliver(readNotebookCache(cacheKey)); });
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
    session->inFlight.insert(msgID, inFlight);
}

NotebookController::KernelSession *NotebookController::ensureKernel(const RunRequest &request,
                                                                     QString *error)
{
    const QString language = request.language.trimmed().toLower();
    auto it = m_kernels.find(language);
    if (it != m_kernels.end() && it->second->process->isRunning())
        return it->second.get();

    const QHash<QString, KernelSpec> specs = kernelSpecsByLanguage();
    const auto specIt = specs.constFind(language);
    if (specIt == specs.constEnd()) {
        *error = QStringLiteral("no Jupyter kernel installed for language \"%1\"").arg(request.language);
        return nullptr;
    }

    auto session = std::make_unique<KernelSession>();
    session->spec = specIt.value();
    session->tierRequested = request.tier;
    session->process = std::make_unique<KernelProcess>();
    if (!session->process->start(session->spec, request.tier, request.extraReadOnlyBinds)) {
        *error = session->process->error();
        return nullptr;
    }

    session->client = std::make_unique<KernelClient>();
    KernelSession *raw = session.get();
    wireKernel(raw);
    session->client->startClient(session->process->connectionInfo(),
                                 QUuid::createUuid().toString(QUuid::WithoutBraces),
                                 QStringLiteral("noteAppC"));

    m_kernels.emplace(language, std::move(session));
    emit kernelStateChanged(language, true);
    return raw;
}

void NotebookController::wireKernel(KernelSession *session)
{
    KernelClient *client = session->client.get();

    connect(client, &KernelClient::streamOutput, this,
           [session](const QString &parent, const QString &, const QString &text) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               // Stream text accumulates into one Text output, first in the
               // list - the order a person reading a notebook expects
               // (what print() said, then the cell's own result).
               if (it->result.outputs.isEmpty()
                   || it->result.outputs.constFirst().kind != NotebookOutput::Kind::Text) {
                   NotebookOutput output;
                   output.kind = NotebookOutput::Kind::Text;
                   it->result.outputs.prepend(output);
               }
               it->result.outputs[0].text += text;
           });
    connect(client, &KernelClient::executeResultReceived, this,
           [session](const QString &parent, const QJsonObject &data, int) {
               auto it = session->inFlight.find(parent);
               if (it != session->inFlight.end())
                   it->result.outputs.append(outputFromMimeBundle(data));
           });
    connect(client, &KernelClient::displayDataReceived, this,
           [session](const QString &parent, const QJsonObject &data) {
               auto it = session->inFlight.find(parent);
               if (it != session->inFlight.end())
                   it->result.outputs.append(outputFromMimeBundle(data));
           });
    connect(client, &KernelClient::errorReceived, this,
           [session](const QString &parent, const QString &ename, const QString &evalue,
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
               it->result.outputs.append(output);
           });
    connect(client, &KernelClient::executeReplyReceived, this,
           [this, session](const QString &parent, bool ok, int) {
               auto it = session->inFlight.find(parent);
               if (it == session->inFlight.end())
                   return;
               NotebookRunResult result = it->result;
               result.hasRun = true;
               result.ok = result.ok && ok;
               result.wallMs = it->clock.elapsed();
               writeNotebookCache(it->cacheKey, result);
               const Deliver deliver = it->deliver;
               session->inFlight.erase(it);
               deliver(result);
           });
}

void NotebookController::failInFlight(KernelSession *session, const QString &error)
{
    const QHash<QString, InFlight> pending = session->inFlight;
    session->inFlight.clear();
    for (const InFlight &entry : pending) {
        NotebookRunResult result;
        result.kernelError = error;
        entry.deliver(result);
    }
}

void NotebookController::interruptKernel(const QString &language)
{
    const auto it = m_kernels.find(language.trimmed().toLower());
    if (it == m_kernels.end())
        return;
    KernelSession *session = it->second.get();
    session->process->interrupt();          // "signal" mode: what python3's kernelspec uses
    session->client->requestInterrupt();     // "message" mode: harmless if unsupported
}

void NotebookController::restartKernel(const QString &language)
{
    const QString key = language.trimmed().toLower();
    const auto it = m_kernels.find(key);
    if (it == m_kernels.end())
        return;
    KernelSession *session = it->second.get();
    failInFlight(session, QStringLiteral("kernel restarted"));
    m_kernels.erase(it); // ~KernelSession stops the client thread and kills the process
    emit kernelStateChanged(key, false);
}

void NotebookController::shutdownAll()
{
    for (auto &entry : m_kernels)
        failInFlight(entry.second.get(), QStringLiteral("kernel shut down"));
    m_kernels.clear();
}

bool NotebookController::hasRunningKernel(const QString &language) const
{
    const auto it = m_kernels.find(language.trimmed().toLower());
    return it != m_kernels.end() && it->second->process->isRunning();
}

} // namespace noteAppC::notebook
