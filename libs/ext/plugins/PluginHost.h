#pragma once

#include "PluginManifest.h"
#include "PluginProtocol.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>

#include <functional>
#include <memory>

class QProcess;
class QTimer;

namespace noteAppC {

/// Callbacks PluginHost calls to fulfil a host-table request a plugin made
/// (plugin.h's noteappc_host), each gated by the PluginPermission named in
/// its comment - PluginHost checks the permission itself before calling any
/// of these, so a callback never has to re-check. Supplied once by the app
/// (MainWindow) at construction; PluginHost never touches the vault, the
/// editor or the window directly - see docs/PLUGINS.md, "the host gets data
/// only through the protocol".
struct PluginAppCallbacks {
    std::function<QString()> activeNotePath;                    ///< read-notes
    std::function<QString()> activeNoteText;                    ///< read-notes
    std::function<QString()> activeSelectionText;                ///< read-notes; empty = no selection/refused
    std::function<QString(const QString &vaultPath)> readNote;  ///< read-notes; empty = missing/refused
    std::function<QStringList()> listNotes;                     ///< read-notes
    std::function<bool(const QString &utf8)> replaceSelection;  ///< write-notes
    std::function<bool(const QString &vaultPath, const QString &utf8)> writeNote; ///< write-notes
    std::function<QString()> clipboardRead;                     ///< clipboard
    std::function<bool(const QString &utf8)> clipboardWrite;    ///< clipboard
    std::function<void(const QString &message)> showNotice;     ///< always allowed
    std::function<void(const QString &pluginID, const QString &message)> log; ///< always allowed
};

/// One running (or not yet started) `noteAppC-plugin-host` process for one
/// enabled plugin (docs/PLUGINS.md, Tier 2). Owns the QProcess, the sandbox
/// and rlimit setup, the framed protocol on both ends, permission
/// enforcement on every host->app request, per-request timeouts and
/// crash/backoff restart.
///
/// MainWindow (or PluginRegistry on its behalf) owns one of these per
/// enabled plugin and only calls ensureStarted()/runCommand()/
/// renderCodeBlock()/notifyVaultEvent()/stop() - the rest is internal.
/// Nothing here starts a process by itself; the first call to any of the
/// three request methods is what does it (lazy, after the first paint).
class PluginHost : public QObject {
    Q_OBJECT
public:
    PluginHost(PluginManifest manifest, QSet<PluginPermission> grantedPermissions,
              PluginAppCallbacks callbacks, QObject *parent = nullptr);
    ~PluginHost() override;

    const PluginManifest &manifest() const { return m_manifest; }

    /// Starts the host process if it is not already running/starting and
    /// the crash backoff has elapsed; a no-op otherwise. Idempotent, cheap
    /// to call before every request.
    void ensureStarted();
    /// Sends a "shutdown" notification, gives the process a moment to exit
    /// on its own, then kills it. Safe to call whether or not it is
    /// running.
    void stop();

    using ResultCallback = std::function<void(bool ok, const QJsonObject &result, const QString &error)>;
    /// `commandID` is bare (no "pluginID:" prefix - the app adds that when
    /// registering the QAction; see docs/PLUGINS.md's MainWindow snippet).
    void runCommand(const QString &commandID, const ResultCallback &callback);
    void renderCodeBlock(const QString &language, const QString &source, int widthPx,
                         double devicePixelRatio, const QString &theme, const ResultCallback &callback);
    /// Fire-and-forget, and never itself a lazy-start trigger: PluginRegistry
    /// only calls this on a host it has already started because the plugin
    /// declared and was woken for this event.
    void notifyVaultEvent(const QString &event, const QString &path);

    enum class State { Stopped, Starting, Running, Crashed };
    State state() const { return m_state; }

    /// What the settings page shows per plugin: the last measured round
    /// trips, and whether the plugin is flagged slow - a command over
    /// 100 ms or a render over 1000 ms (docs/PLUGINS.md's stated budgets).
    struct Timing {
        qint64 lastStartMs = -1;
        qint64 lastCommandMs = -1;
        qint64 lastRenderMs = -1;
        int consecutiveCrashes = 0;
        bool slow = false;
    };
    Timing timing() const { return m_timing; }

    /// Overrides the hard per-request kill timeout - the constructor already
    /// set it from the manifest's `[plugin] timeout-ms` (PluginManifest::
    /// requestTimeoutMs(), clamped to [1000, 300000]) if it declared one,
    /// default 4000 otherwise. Production code never calls this; it exists
    /// so tests/test_plugins_host.cpp does not need several real seconds -
    /// or several real minutes, for a plugin that declared a long one - to
    /// prove a hang is killed.
    void setRequestTimeoutMsForTesting(int ms) { m_requestTimeoutMs = ms; }

signals:
    void stateChanged(State state);
    /// A request was refused for lacking a granted permission - the
    /// settings page and the app log both want to know (docs/PLUGINS.md: "a
    /// request outside the granted set is refused with an error, and
    /// logged").
    void permissionRefused(const QString &method, const QString &permission);

private:
    void handleReadyRead();
    void handleFinished(int exitCode, int exitStatus);
    void dispatchIncoming(const PluginMessage &message);
    void handleHostRequest(const PluginMessage &request);
    void sendMessage(const PluginMessage &message);
    qint64 sendRequest(const QString &method, const QJsonObject &params, const ResultCallback &callback);
    void failAllPending(const QString &error);
    void setState(State state);
    bool permissionGranted(PluginPermission permission) const;
    void refuseHostRequest(qint64 requestId, const QString &method, PluginPermission permission);
    QStringList sandboxArguments(QString *program) const;

    static QString hostBinaryPath();

    PluginManifest m_manifest;
    QSet<PluginPermission> m_granted;
    PluginAppCallbacks m_callbacks;
    std::unique_ptr<QProcess> m_process;
    PluginFrameReader m_reader;
    State m_state = State::Stopped;
    qint64 m_nextRequestID = 1;

    struct Pending {
        ResultCallback callback;
        QTimer *timer = nullptr;
        QElapsedTimer clock;
        QString method;
    };
    QHash<qint64, Pending> m_pending;

    Timing m_timing;
    QElapsedTimer m_sinceFailure;
    bool m_hasFailed = false;
    // Default for a plugin whose manifest declares no `timeout-ms` - the
    // constructor body overrides this from PluginManifest::requestTimeoutMs()
    // when one is declared (docs/PLUGINS.md, "Timeouts": an AI-style plugin
    // whose calls take seconds to minutes needs far more than this).
    int m_requestTimeoutMs = 4000;
};

} // namespace noteAppC
