// noteAppC-plugin-host --plugin <dir>
//
// One process per enabled plugin (docs/PLUGINS.md, Tier 2), spawned by
// libs/ext/plugins/PluginHost.cpp, usually under bubblewrap. Everything this
// binary does is: parse plugin.ini, load the plugin through an Engine
// (Engine.h - NativeEngine today, dlopen and the C ABI in plugin.h), and
// pump the framed protocol (HostIO.h) until told to stop. It links only
// Qt6::Core and the protocol library - no widgets, no vault, no editor - so
// a crash or a hang here is contained to this one process and this one
// plugin.

#include "Engine.h"
#include "HostIO.h"
#include "NativeEngine.h"

#include <PluginManifest.h>
#include <PluginProtocol.h>

#include <noteappc/plugin.h>

#include <QCoreApplication>
#include <QJsonArray>

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace noteAppC;

namespace {

HostIO *g_io = nullptr;

// Every noteappc_host function that returns a `const char *` follows the
// getenv() convention plugin.h documents: valid until the next call to any
// host-table function. A `static QByteArray` per function is what makes
// that hold - each has its own slot, so one plugin doing
// `active_note_path(); active_note_text();` in sequence and needing both
// strings still works if it copies the first before calling the second, and
// nothing shares a buffer across two different functions.

const char *host_active_note_path()
{
    static QByteArray buffer;
    const PluginMessage response = g_io->request(QStringLiteral("note.activePath"), {});
    buffer = response.ok ? response.result.value(QStringLiteral("path")).toString().toUtf8() : QByteArray();
    return buffer.constData();
}

const char *host_active_note_text()
{
    static QByteArray buffer;
    const PluginMessage response = g_io->request(QStringLiteral("note.activeText"), {});
    buffer = response.ok ? response.result.value(QStringLiteral("text")).toString().toUtf8() : QByteArray();
    return buffer.constData();
}

const char *host_read_note(const char *vaultPath)
{
    static QByteArray buffer;
    QJsonObject params;
    params.insert(QStringLiteral("path"), QString::fromUtf8(vaultPath ? vaultPath : ""));
    const PluginMessage response = g_io->request(QStringLiteral("note.read"), params);
    buffer = response.ok ? response.result.value(QStringLiteral("text")).toString().toUtf8() : QByteArray();
    return buffer.constData();
}

const char *host_list_notes()
{
    // NUL-separated, double-NUL-terminated (plugin.h's documented shape):
    // each path followed by one NUL, then one more NUL closes the list - an
    // empty list is exactly that closing NUL on its own.
    static QByteArray buffer;
    buffer.clear();
    const PluginMessage response = g_io->request(QStringLiteral("note.list"), {});
    if (response.ok) {
        for (const QJsonValue &value : response.result.value(QStringLiteral("paths")).toArray()) {
            buffer += value.toString().toUtf8();
            buffer += '\0';
        }
    }
    buffer += '\0';
    return buffer.constData();
}

int host_replace_selection(const char *utf8)
{
    QJsonObject params;
    params.insert(QStringLiteral("text"), QString::fromUtf8(utf8 ? utf8 : ""));
    const PluginMessage response = g_io->request(QStringLiteral("note.replaceSelection"), params);
    return (response.ok && response.result.value(QStringLiteral("ok")).toBool()) ? 0 : 1;
}

int host_write_note(const char *vaultPath, const char *utf8)
{
    QJsonObject params;
    params.insert(QStringLiteral("path"), QString::fromUtf8(vaultPath ? vaultPath : ""));
    params.insert(QStringLiteral("text"), QString::fromUtf8(utf8 ? utf8 : ""));
    const PluginMessage response = g_io->request(QStringLiteral("note.write"), params);
    return (response.ok && response.result.value(QStringLiteral("ok")).toBool()) ? 0 : 1;
}

const char *host_clipboard_read()
{
    static QByteArray buffer;
    const PluginMessage response = g_io->request(QStringLiteral("clipboard.read"), {});
    buffer = response.ok ? response.result.value(QStringLiteral("text")).toString().toUtf8() : QByteArray();
    return buffer.constData();
}

int host_clipboard_write(const char *utf8)
{
    QJsonObject params;
    params.insert(QStringLiteral("text"), QString::fromUtf8(utf8 ? utf8 : ""));
    const PluginMessage response = g_io->request(QStringLiteral("clipboard.write"), params);
    return (response.ok && response.result.value(QStringLiteral("ok")).toBool()) ? 0 : 1;
}

void host_show_notice(const char *utf8)
{
    QJsonObject params;
    params.insert(QStringLiteral("message"), QString::fromUtf8(utf8 ? utf8 : ""));
    g_io->writeMessage(PluginMessage::notification(QStringLiteral("notice.show"), params));
}

void host_log(const char *utf8)
{
    QJsonObject params;
    params.insert(QStringLiteral("message"), QString::fromUtf8(utf8 ? utf8 : ""));
    g_io->writeMessage(PluginMessage::notification(QStringLiteral("log"), params));
}

const noteappc_host kHostTable = {
    NOTEAPPC_PLUGIN_API,
    host_active_note_path,
    host_active_note_text,
    host_read_note,
    host_list_notes,
    host_replace_selection,
    host_write_note,
    host_clipboard_read,
    host_clipboard_write,
    host_show_notice,
    host_log,
};

/// Handles one "command.run" or "codeBlock.render" request from the app,
/// replying on `io` either way - the one place main() needs to know both
/// method names, everything else is dispatch.
void handleAppRequest(HostIO &io, Engine &engine, const PluginMessage &request)
{
    if (request.method == QLatin1String("command.run")) {
        const QString commandID = request.params.value(QStringLiteral("commandID")).toString();
        const int rc = engine.runCommand(commandID);
        if (rc == 0)
            io.writeMessage(PluginMessage::response(request.id));
        else
            io.writeMessage(PluginMessage::errorResponse(request.id, QStringLiteral("command-failed"),
                                                          QStringLiteral("run_command returned %1").arg(rc)));
        return;
    }
    if (request.method == QLatin1String("codeBlock.render")) {
        const QString language = request.params.value(QStringLiteral("language")).toString();
        const QString source = request.params.value(QStringLiteral("source")).toString();
        const int widthPx = request.params.value(QStringLiteral("width")).toInt();
        const float dpr = static_cast<float>(request.params.value(QStringLiteral("devicePixelRatio")).toDouble(1.0));

        noteappc_block_result out{};
        const int rc = engine.renderCodeBlock(language, source, widthPx, dpr, &out);
        if (rc == 0 && (out.text || out.image.rgba)) {
            QJsonObject result;
            if (out.image.rgba) {
                result.insert(QStringLiteral("isMarkdown"), false);
                result.insert(QStringLiteral("width"), out.image.width);
                result.insert(QStringLiteral("height"), out.image.height);
                const qsizetype bytes = qsizetype(out.image.width) * out.image.height * 4;
                result.insert(QStringLiteral("rgbaBase64"),
                             QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(out.image.rgba),
                                                            int(bytes))
                                                     .toBase64()));
            } else {
                result.insert(QStringLiteral("isMarkdown"), true);
                result.insert(QStringLiteral("text"), QString::fromUtf8(out.text));
            }
            io.writeMessage(PluginMessage::response(request.id, result));
        } else {
            io.writeMessage(PluginMessage::errorResponse(request.id, QStringLiteral("render-failed"),
                                                          QStringLiteral("render_code_block returned %1").arg(rc)));
        }
        std::free(out.text);
        std::free(out.image.rgba);
        return;
    }
    io.writeMessage(PluginMessage::errorResponse(request.id, QStringLiteral("unknown-method"),
                                                 QStringLiteral("unknown method ") + request.method));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); // QJson*/QString need no event loop, but this keeps Qt's own bookkeeping happy
    Q_UNUSED(app);

    QString pluginDirectory;
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == QLatin1String("--plugin") && i + 1 < argc)
            pluginDirectory = QString::fromLocal8Bit(argv[++i]);
    }
    if (pluginDirectory.isEmpty()) {
        std::fprintf(stderr, "usage: noteAppC-plugin-host --plugin <directory>\n");
        return 2;
    }

    HostIO io;
    g_io = &io;

    const PluginManifest manifest = PluginManifest::load(pluginDirectory);
    std::unique_ptr<Engine> engine;
    QString loadError;
    if (!manifest.isValid()) {
        loadError = manifest.error();
    } else if (manifest.engine() == PluginEngineKind::QuickJS) {
        // Not built - see tools/plugin_host/QuickJSEngine.h for the seam
        // and the root CMakeLists.txt's NOTEAPPC_WITH_QUICKJS option, which
        // refuses to configure a build that would ever reach this branch
        // expecting it to work.
        loadError = QStringLiteral("engine = quickjs is not built into this noteAppC (docs/PLUGINS.md)");
    } else {
        auto native = std::make_unique<NativeEngine>();
        if (!native->load(manifest, &kHostTable, &loadError))
            native.reset();
        engine = std::move(native);
    }

    // The app always sends "init" first; whether or not the plugin actually
    // loaded, it gets a real answer instead of the host just vanishing -
    // PluginHost::ensureStarted()'s pending "init" request would otherwise
    // only learn about this from the process exiting, which is a worse
    // error message on the settings page than the real one.
    PluginMessage initRequest;
    if (!io.readMessage(&initRequest) || initRequest.method != QLatin1String("init")) {
        std::fprintf(stderr, "noteAppC-plugin-host: expected an init request first\n");
        return 1;
    }
    if (!engine) {
        io.writeMessage(PluginMessage::errorResponse(initRequest.id, QStringLiteral("load-failed"), loadError));
        return 1;
    }
    io.writeMessage(PluginMessage::response(initRequest.id));

    while (true) {
        PluginMessage message;
        if (!io.readMessage(&message))
            break; // EOF/oversized frame - the app is gone or the stream is unusable either way
        if (message.kind == PluginMessage::Kind::Notification) {
            if (message.method == QLatin1String("shutdown"))
                break;
            if (message.method == QLatin1String("vault.event")) {
                engine->vaultEvent(message.params.value(QStringLiteral("event")).toString(),
                                  message.params.value(QStringLiteral("path")).toString());
            }
            continue;
        }
        if (message.kind != PluginMessage::Kind::Request)
            continue; // a stray response with nothing waiting on it - ignore
        handleAppRequest(io, *engine, message);
    }

    engine->unload();
    return 0;
}
