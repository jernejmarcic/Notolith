#include "SyncEngine.h"

#include <algorithm>
#include <atomic>

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTimer>

#include <qt6keychain/keychain.h>

#include "SyncCrypto.h"
#include "SyncJournal.h"
#include "SyncMerge.h"

namespace noteAppC {

namespace {

constexpr qsizetype kInlineBodyLimit = 16 * 1024; ///< docs/SYNC.md "Record and blob formats"
constexpr int kFetchStalls = 3;  ///< cycles an unfetchable blob may hold the pull back
constexpr int kDeleteStalls = 3; ///< cycles a remote delete that cannot be applied may hold the pull back
const QByteArray kWrapGeneration0 = QByteArrayLiteral("0"); ///< placeholder until itemsKey rotation exists

/// The plaintext structure a journal payload decrypts to - docs/SYNC.md
/// "Record and blob formats".
struct EntityPlain {
    QString entityType;
    QString path;
    QString op; ///< "upsert" | "delete"
    qint64 mtime = 0;
    QString body;           ///< inline note text, when blobAddress is empty
    QByteArray blobAddress; ///< set instead of body for attachments and oversized notes
    /// SyncCrypto::digest of the state the writer built on (empty: a new
    /// entity, or a writer from before parents), and of this record's own
    /// content (note: UTF-8 of the LF-normalised text; attachment: the raw
    /// bytes; empty for a delete). docs/SYNC.md "Record and blob formats".
    QByteArray parent;
    QByteArray hash;
};

/// Test seam: a writer from before records carried parent and hash.
std::atomic<bool> g_legacyRecordFormat{false};

QByteArray serialize(const EntityPlain &e)
{
    QJsonObject o{{QStringLiteral("entityType"), e.entityType},
                 {QStringLiteral("path"), e.path},
                 {QStringLiteral("op"), e.op},
                 {QStringLiteral("mtime"), double(e.mtime)}};
    if (!e.blobAddress.isEmpty())
        o.insert(QStringLiteral("blobAddress"), QString::fromLatin1(e.blobAddress.toBase64()));
    else
        o.insert(QStringLiteral("body"), e.body);
    if (!e.parent.isEmpty() && !g_legacyRecordFormat)
        o.insert(QStringLiteral("parent"), QString::fromLatin1(e.parent.toBase64()));
    if (!e.hash.isEmpty() && !g_legacyRecordFormat)
        o.insert(QStringLiteral("hash"), QString::fromLatin1(e.hash.toBase64()));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

EntityPlain parse(const QByteArray &json, bool *ok)
{
    EntityPlain e;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (ok)
            *ok = false;
        return e;
    }
    const QJsonObject o = doc.object();
    e.entityType = o.value(QStringLiteral("entityType")).toString();
    e.path = o.value(QStringLiteral("path")).toString();
    e.op = o.value(QStringLiteral("op")).toString();
    e.mtime = qint64(o.value(QStringLiteral("mtime")).toDouble());
    if (o.contains(QStringLiteral("blobAddress")))
        e.blobAddress = QByteArray::fromBase64(o.value(QStringLiteral("blobAddress")).toString().toLatin1());
    else
        e.body = o.value(QStringLiteral("body")).toString();
    if (o.contains(QStringLiteral("parent")))
        e.parent = QByteArray::fromBase64(o.value(QStringLiteral("parent")).toString().toLatin1());
    if (o.contains(QStringLiteral("hash")))
        e.hash = QByteArray::fromBase64(o.value(QStringLiteral("hash")).toString().toLatin1());
    if (ok)
        *ok = !e.entityType.isEmpty() && !e.path.isEmpty() && !e.op.isEmpty();
    return e;
}

/// "<name> (sync conflict, <device>, <timestamp>).md" - docs/SYNC.md
/// "Conflict rules".
QString conflictCopyPath(const Vault &vault, const QString &relativePath, const QByteArray &deviceID)
{
    const QFileInfo info(relativePath);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    const QString tag = QString::fromLatin1(deviceID.left(4).toHex());
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QString dirPrefix = info.path() == QStringLiteral(".") ? QString() : info.path() + QLatin1Char('/');
    return vault.absolute(QStringLiteral("%1%2 (sync conflict, %3, %4)%5")
                              .arg(dirPrefix, info.completeBaseName(), tag, stamp, suffix));
}

bool writeRawFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(bytes);
    return out.commit();
}

/// Runs a QKeychain job to completion on the calling thread's own event
/// loop (SyncEngine's worker thread - see the class comment in
/// SyncEngine.h) rather than wiring up finished()/error() by hand for one
/// call site each. A watchdog timeout, matching SyncClient::callRaw()'s
/// own 15 s one: an unresponsive secret-service D-Bus backend must not
/// hang the sync thread forever (and, since this runs inside runCycle()'s
/// m_inCycle window, must not hang the destructor's teardown either).
SyncEngine::SecretStore &secretStore()
{
    static SyncEngine::SecretStore store;
    return store;
}

void runKeychainJob(QKeychain::Job *job)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    // ponytail: on a genuine timeout the stack-allocated job in
    // loadDeviceIdentity() is destroyed with its D-Bus call still
    // in flight - a late reply is simply dropped (QKeychain does not
    // deliver into a destroyed QObject). Upgrade to a heap-allocated,
    // deleteLater()-based job if a backend that hangs this long in
    // practice turns up.
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
}

const QString kKeychainService = QStringLiteral("noteAppC-sync");

bool secretRead(const QString &key, QByteArray *value)
{
    const SyncEngine::SecretStore &store = secretStore();
    if (store.read && store.write)
        return store.read(key, value);
    QKeychain::ReadPasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);
    runKeychainJob(&job);
    // An empty value is how a watchdog timeout looks (the job never
    // finished, error() is still NoError); no secret here is ever empty.
    if (job.error() != QKeychain::NoError || job.binaryData().isEmpty())
        return false;
    *value = job.binaryData();
    return true;
}

bool secretWrite(const QString &key, const QByteArray &value)
{
    const SyncEngine::SecretStore &store = secretStore();
    if (store.read && store.write)
        return store.write(key, value);
    QKeychain::WritePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);
    job.setBinaryData(value);
    runKeychainJob(&job);
    return job.error() == QKeychain::NoError;
}

bool secretRemove(const QString &key)
{
    const SyncEngine::SecretStore &store = secretStore();
    if (store.read && store.write)
        return store.remove && store.remove(key);
    QKeychain::DeletePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);
    runKeychainJob(&job);
    return job.error() == QKeychain::NoError;
}

} // namespace

void SyncEngine::setSecretStore(SecretStore store)
{
    secretStore() = std::move(store);
}

void SyncEngine::setLegacyRecordFormatForTests(bool legacy)
{
    g_legacyRecordFormat = legacy;
}

SyncEngine::SyncEngine()
{
    qRegisterMetaType<Status>();
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(4000); // a few seconds: batches a burst of edits into one scan
    connect(m_debounce, &QTimer::timeout, this, &SyncEngine::runCycle);

    m_periodic = new QTimer(this);
    m_periodic->setInterval(120000); // catches remote changes even with no local edits
    connect(m_periodic, &QTimer::timeout, this, &SyncEngine::runCycle);

    m_thread.setObjectName(QStringLiteral("noteAppC-sync"));
    moveToThread(&m_thread);
    m_thread.start();
}

SyncEngine::~SyncEngine()
{
    QMetaObject::invokeMethod(this, &SyncEngine::stopOnWorker, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void SyncEngine::start(const QString &vaultRoot, const QString &serverURL, const QString &email,
                       PasswordPrompt prompt, bool viaLockbox)
{
    QMetaObject::invokeMethod(
        this, [this, vaultRoot, serverURL, email, prompt, viaLockbox] {
            startOnWorker(vaultRoot, serverURL, email, prompt, viaLockbox);
        },
        Qt::QueuedConnection);
}

void SyncEngine::stop()
{
    QMetaObject::invokeMethod(this, &SyncEngine::stopOnWorker, Qt::QueuedConnection);
}

void SyncEngine::noteChanged()
{
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (m_running)
                m_debounce->start();
        },
        Qt::QueuedConnection);
}

void SyncEngine::syncNow()
{
    QMetaObject::invokeMethod(this, &SyncEngine::runCycle, Qt::QueuedConnection);
}

void SyncEngine::setStatus(Status status, const QString &message)
{
    emit statusChanged(status, message);
}

void SyncEngine::startOnWorker(QString vaultRoot, QString serverURL, QString email, PasswordPrompt prompt,
                               bool viaLockbox)
{
    stopOnWorker(); // switching vaults (or restarting) tears down the old session first
    if (!SyncCrypto::init()) {
        setStatus(Status::Error, QStringLiteral("libsodium failed to initialise"));
        return;
    }
    m_vault = Vault(vaultRoot);
    if (!m_vault.isValid()) {
        setStatus(Status::Error, QStringLiteral("not a vault"));
        return;
    }
    m_journal = std::make_unique<SyncJournal>(vaultRoot);
    if (!m_journal->isOpen()) {
        setStatus(Status::Error, m_journal->error());
        m_journal.reset();
        return;
    }
    m_client = std::make_unique<SyncClient>(serverURL, this);
    m_email = email;
    m_serverURL = serverURL.trimmed();
    while (m_serverURL.endsWith(u'/'))
        m_serverURL.chop(1);
    m_storeTried = false;
    m_keysFromStore = false;
    m_forgotten = false;
    m_refusedPaths.clear();
    m_unreadable.clear();
    m_fetchStalls.clear();
    m_prompt = std::move(prompt);
    m_viaLockbox = viaLockbox;
    m_running = true;
    m_periodic->start();
    setStatus(Status::Idle);
    runCycle();
}

void SyncEngine::stopOnWorker()
{
    m_debounce->stop();
    m_periodic->stop();
    m_running = false; // checked at the top of runCycle() and by the caller below
    if (m_inCycle) {
        // A cycle is paused right now inside a nested event loop
        // (SyncClient's HTTP wait, or loadDeviceIdentity()'s keychain
        // wait) on this very thread - reentrant, because that nested loop
        // pumps this thread's whole event queue, including whatever
        // queued or blocking-queued call reached stopOnWorker() itself.
        // Tearing down m_journal/m_client here would yank them out from
        // under the call chain that is about to resume and use them
        // (caught under AddressSanitizer - see the m_inCycle comment in
        // SyncEngine.h). m_running=false above is enough for now: the
        // cycle's steps never re-check it mid-flight, so it runs to a
        // harmless completion (nothing left to schedule - the timers are
        // already stopped) and calls stopOnWorker() again itself, below,
        // finishing the teardown for real once m_inCycle is false.
        return;
    }
    SyncCrypto::wipe(m_loginKey);
    SyncCrypto::wipe(m_wrapKey);
    SyncCrypto::wipe(m_contentKey);
    SyncCrypto::wipe(m_boxSecretKey);
    m_loginKey.clear();
    m_wrapKey.clear();
    m_contentKey.clear();
    m_boxSecretKey.clear();
    m_boxPublicKey.clear();
    m_vaultID.clear();
    m_announced.clear();
    m_client.reset();
    m_journal.reset();
    setStatus(m_forgotten ? Status::NeedsPassword : Status::Idle);
}

void SyncEngine::runCycle()
{
    if (!m_running || m_inCycle)
        return;
    m_inCycle = true;
    const auto done = qScopeGuard([this] {
        m_inCycle = false;
        if (!m_running)
            stopOnWorker(); // deferred from a reentrant stop() while this cycle was in flight
    });
    setStatus(Status::SigningIn);
    if (!ensureSession())
        return; // status already set inside
    if (!ensureItemsKey())
        return;

    setStatus(Status::Scanning);
    const QList<SyncScanner::Change> changes = SyncScanner::scan(m_vault, *m_journal, m_contentKey);
    for (const SyncScanner::Change &change : changes)
        queueLocalChange(change);

    setStatus(Status::Syncing);
    const int pushed = pushOutbox();
    int pulled = 0;
    int conflicts = 0;
    loadPendingDeletes();
    retryPendingDeletes();
    const QString pullError = pullJournal(&pulled, &conflicts);

    maintainEnrolment();
    QStringList warnings;
    for (const QString &w : {pendingDeleteWarning(), refusedWarning(), unreadableWarning()}) {
        if (!w.isEmpty())
            warnings << w;
    }
    const QString warning = warnings.join(QStringLiteral("; "));
    if (!pullError.isEmpty())
        setStatus(Status::Error, pullError);
    else
        setStatus(Status::Idle, warning); // a parked delete is not a failure; the message stays until it goes through
    emit syncFinished(pushed, pulled, conflicts);
}

bool SyncEngine::deriveKeysFromPassword(QByteArray *salt, bool refreshSalt)
{
    // The password first, the network second: with nobody to ask there is
    // nothing to contact the server for.
    const QString password = m_prompt ? m_prompt(m_email) : QString();
    if (password.isEmpty()) {
        setStatus(Status::NeedsPassword, QStringLiteral("no password given"));
        return false;
    }
    if (salt->isEmpty() || refreshSalt) {
        const SyncClient::Salt fetched = m_client->fetchSalt(m_email);
        if (fetched.ok)
            *salt = fetched.salt;
        else if (salt->isEmpty()) {
            setStatus(Status::Error, QStringLiteral("could not reach the server"));
            return false;
        }
    }
    QByteArray masterKey = SyncCrypto::deriveMasterKey(password.toUtf8(), *salt);
    if (masterKey.isEmpty()) {
        setStatus(Status::Error, QStringLiteral("key derivation failed"));
        return false;
    }
    m_loginKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextLogin);
    m_wrapKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextWrap);
    m_contentKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextContent);
    SyncCrypto::wipe(masterKey);
    return true;
}

// contentKey is a per-device secret that only keys this device's own change
// detection and blob addresses (docs/SYNC.md "Content addresses"): no other
// device needs the same bytes, so it is seeded once from the derived key
// (hashes already on file stay valid) and then never follows the password.
void SyncEngine::adoptContentKey()
{
    const QByteArray stored = m_journal->meta(QStringLiteral("contentKey"));
    if (stored.size() == SyncCrypto::kKeyBytes)
        m_contentKey = stored;
    else
        m_journal->setMeta(QStringLiteral("contentKey"), m_contentKey);
}

bool SyncEngine::ensureSession()
{
    if (!m_client->token().isEmpty() && !m_vaultID.isEmpty())
        return true;

    QByteArray salt = m_journal->meta(QStringLiteral("salt"));
    if (m_wrapKey.isEmpty() && !m_storeTried) {
        m_storeTried = true; // one keyring read per start, not one per cycle
        m_keysFromStore = loadStoredKeys();
    }
    if (m_wrapKey.isEmpty() && !deriveKeysFromPassword(&salt, false))
        return false;
    if (salt.isEmpty()) { // keys from the keyring and no salt on file: only account registration needs one
        const SyncClient::Salt fetched = m_client->fetchSalt(m_email);
        if (!fetched.ok) {
            setStatus(Status::Error, QStringLiteral("could not reach the server"));
            return false;
        }
        salt = fetched.salt;
    }

    SyncClient::Auth auth = m_client->signIn(m_email, m_loginKey);
    if (!auth.ok && m_keysFromStore && auth.status != 0) {
        // The server answered and said no: the stored keys are stale (the
        // password was changed on another device). An unreachable server
        // (status 0) keeps them - offline is not a reason to forget.
        dropStoredKeys();
        m_keysFromStore = false;
        SyncCrypto::wipe(m_loginKey);
        SyncCrypto::wipe(m_wrapKey);
        SyncCrypto::wipe(m_contentKey);
        m_loginKey.clear();
        m_wrapKey.clear();
        m_contentKey.clear();
        if (!deriveKeysFromPassword(&salt, true)) // a password change also changed the salt
            return false;
        auth = m_client->signIn(m_email, m_loginKey);
    }
    if (!auth.ok)
        auth = m_client->registerAccount(m_email, salt, m_loginKey);
    if (!auth.ok) {
        setStatus(Status::Error, auth.error.isEmpty() ? QStringLiteral("sign-in failed") : auth.error);
        return false;
    }
    adoptContentKey();
    if (!m_keysFromStore) {
        // Only keys the server just accepted are worth keeping.
        storeKeys();
        m_keysFromStore = true;
    }
    m_client->setToken(auth.token);
    m_journal->setMeta(QStringLiteral("salt"), salt);

    QByteArray vaultID = m_journal->meta(QStringLiteral("vaultID"));
    if (vaultID.isEmpty()) {
        const QList<QByteArray> existing = m_client->listVaults();
        if (!existing.isEmpty()) {
            vaultID = existing.first();
        } else if (!m_viaLockbox) {
            const SyncClient::Created created = m_client->createVault();
            if (!created.ok) {
                setStatus(Status::Error, QStringLiteral("could not create the vault on the server"));
                return false;
            }
            vaultID = created.id;
        } else {
            setStatus(Status::WaitingForApproval, QStringLiteral("no vault to enrol into yet"));
            return false;
        }
        m_journal->setMeta(QStringLiteral("vaultID"), vaultID);
    }
    m_vaultID = vaultID;
    return true;
}

bool SyncEngine::loadDeviceIdentity()
{
    if (!m_boxPublicKey.isEmpty() && !m_boxSecretKey.isEmpty())
        return true;

    const bool haveCachedPublic = !m_journal->meta(QStringLiteral("boxPublicKey")).isEmpty();
    const QString keychainKey = QStringLiteral("device-box-secret-") + QString::fromLatin1(m_journal->deviceID().toHex());

    QByteArray secret;
    if (haveCachedPublic && secretRead(keychainKey, &secret)) {
        m_boxPublicKey = m_journal->meta(QStringLiteral("boxPublicKey"));
        m_boxSecretKey = secret;
        return true;
    }

    // Nothing usable in the keyring yet: mint a fresh device identity.
    const SyncCrypto::BoxKeyPair pair = SyncCrypto::generateBoxKeyPair();
    m_boxPublicKey = pair.publicKey;
    m_boxSecretKey = pair.secretKey;
    m_journal->setMeta(QStringLiteral("boxPublicKey"), m_boxPublicKey);
    return secretWrite(keychainKey, m_boxSecretKey);
}

// The keys a session derives from the password, kept in the keyring so the
// password is asked once per device (docs/SYNC.md "Keys kept in the
// keyring"): one byte of format version, then loginKey, wrapKey, contentKey.
// Runs on the worker thread, like every other keyring call here.
QString SyncEngine::storedKeysName() const
{
    return QStringLiteral("keys/") + m_serverURL + u'/' + QString::fromLatin1(m_journal->deviceID().toHex());
}

bool SyncEngine::loadStoredKeys()
{
    QByteArray blob;
    if (!secretRead(storedKeysName(), &blob))
        return false;
    constexpr int kKey = SyncCrypto::kKeyBytes;
    const bool good = blob.size() == 1 + 3 * kKey && blob.at(0) == 1;
    if (good) {
        m_loginKey = blob.mid(1, kKey);
        m_wrapKey = blob.mid(1 + kKey, kKey);
        m_contentKey = blob.mid(1 + 2 * kKey, kKey);
    }
    SyncCrypto::wipe(blob);
    return good;
}

void SyncEngine::storeKeys() const
{
    QByteArray blob = QByteArray(1, char(1)) + m_loginKey + m_wrapKey + m_contentKey;
    secretWrite(storedKeysName(), blob); // a keyring that refuses just means the password is asked next time
    SyncCrypto::wipe(blob);
}

void SyncEngine::dropStoredKeys() const
{
    secretRemove(storedKeysName());
}

bool SyncEngine::ensureItemsKey()
{
    if (m_journal->activeItemsKey())
        return true;
    if (!loadDeviceIdentity())
        return false;

    const QByteArray deviceID = m_journal->deviceID();
    m_client->registerDevice(m_vaultID, deviceID, m_boxPublicKey, m_viaLockbox); // idempotent

    if (!m_viaLockbox) {
        const QList<SyncClient::WrappedItemsKey> wrapped = m_client->fetchItemsKeys(m_vaultID, deviceID);
        for (const SyncClient::WrappedItemsKey &w : wrapped) {
            bool ok = false;
            const QByteArray plain = SyncCrypto::open(
                m_wrapKey, w.wrapped, SyncCrypto::buildAD("WK", {w.itemsKeyID, kWrapGeneration0}), &ok);
            if (ok)
                m_journal->putItemsKey({w.itemsKeyID, plain, true});
        }
        if (m_journal->activeItemsKey())
            return true;

        // Nobody has uploaded an itemsKey for this vault yet: as far as
        // this device can tell, it is the first one (registerDevice above
        // just added it), so it mints the vault's first itemsKey.
        if (m_client->listDevices(m_vaultID).size() <= 1) {
            const QByteArray itemsKeyID = SyncCrypto::randomID();
            const QByteArray itemsKey = SyncCrypto::randomKey();
            const QByteArray wrap = SyncCrypto::seal(m_wrapKey, itemsKey,
                                                     SyncCrypto::buildAD("WK", {itemsKeyID, kWrapGeneration0}));
            if (!m_client->putItemsKey(m_vaultID, itemsKeyID, wrap))
                return false;
            m_journal->putItemsKey({itemsKeyID, itemsKey, true});
            return true;
        }
        setStatus(Status::WaitingForApproval, QStringLiteral("waiting to be approved by another device"));
        return false;
    }

    // Path 2 (docs/SYNC.md "Device enrolment with lockboxes"): never call
    // the wrapKey-gated endpoint above - wait for an approved device to
    // seal this device a lockbox.
    const QList<SyncClient::Lockbox> boxes = m_client->listLockboxes(m_vaultID, deviceID);
    if (boxes.isEmpty()) {
        // The code to read out on the device that approves it.
        setStatus(Status::WaitingForApproval,
                  QStringLiteral("approve this device on one that already syncs; its code is %1")
                      .arg(SyncCrypto::fingerprint(m_boxPublicKey)));
        return false;
    }
    const SyncCrypto::BoxKeyPair pair{m_boxPublicKey, m_boxSecretKey};
    for (const SyncClient::Lockbox &box : boxes) {
        bool ok = false;
        const QByteArray plain = SyncCrypto::openBox(pair, box.sealed, &ok);
        if (ok)
            m_journal->putItemsKey({box.itemsKeyID, plain, true});
    }
    return m_journal->activeItemsKey().has_value();
}

void SyncEngine::maintainEnrolment()
{
    const auto activeKey = m_journal->activeItemsKey();
    if (!activeKey || !loadDeviceIdentity())
        return;
    const QByteArray deviceID = m_journal->deviceID();

    // The server is not trusted with the keys, so it does not get to say
    // which devices have them either: a device is sealed a lockbox only once
    // a person here approved it, having compared its code. Approvals are
    // kept per vault, on this device only.
    const QList<QByteArray> approved = m_journal->meta(QStringLiteral("approvedDevices")).split('\n');
    for (const SyncClient::DeviceInfo &device : m_client->listDevices(m_vaultID)) {
        if (device.deviceID == deviceID || !device.wantsLockbox)
            continue; // a device with the password takes its keys from that
        const QList<SyncClient::Lockbox> already = m_client->listLockboxes(m_vaultID, device.deviceID);
        QList<SyncJournal::ItemsKeyRecord> missing;
        for (const SyncJournal::ItemsKeyRecord &key : m_journal->allItemsKeys()) {
            if (std::none_of(already.cbegin(), already.cend(),
                             [&key](const SyncClient::Lockbox &box) { return box.itemsKeyID == key.itemsKeyID; }))
                missing.append(key);
        }
        if (missing.isEmpty())
            continue;
        if (!approved.contains(device.deviceID.toHex())) {
            if (!m_announced.contains(device.deviceID)) {
                m_announced.insert(device.deviceID);
                emit deviceAwaitingApproval(device.deviceID, SyncCrypto::fingerprint(device.publicKey));
            }
            continue;
        }
        for (const SyncJournal::ItemsKeyRecord &key : std::as_const(missing))
            m_client->putLockbox(m_vaultID, device.deviceID, key.itemsKeyID,
                                 SyncCrypto::sealToBox(device.publicKey, key.plaintext));
    }
}

void SyncEngine::approveDevice(const QByteArray &deviceID)
{
    QMetaObject::invokeMethod(
        this,
        [this, deviceID] {
            if (!m_running || !m_journal)
                return;
            QByteArray approved = m_journal->meta(QStringLiteral("approvedDevices"));
            if (!approved.split('\n').contains(deviceID.toHex()))
                m_journal->setMeta(QStringLiteral("approvedDevices"),
                                   approved.isEmpty() ? deviceID.toHex() : approved + '\n' + deviceID.toHex());
            if (!m_inCycle)
                maintainEnrolment();
        },
        Qt::QueuedConnection);
}

void SyncEngine::forgetStoredKeys()
{
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!m_running || !m_journal)
                return;
            dropStoredKeys();
            m_forgotten = true;
            stopOnWorker(); // deferred to the end of a cycle in flight
        },
        Qt::QueuedConnection);
}

void SyncEngine::changePassword(const QString &newPassword)
{
    QMetaObject::invokeMethod(
        this,
        [this, newPassword] {
            if (!m_running || m_wrapKey.isEmpty())
                return;
            const QByteArray newSalt = SyncCrypto::randomSalt();
            QByteArray newMaster = SyncCrypto::deriveMasterKey(newPassword.toUtf8(), newSalt);
            if (newMaster.isEmpty())
                return;
            const QByteArray newLoginKey = SyncCrypto::deriveSubkey(newMaster, SyncCrypto::kContextLogin);
            const QByteArray newWrapKey = SyncCrypto::deriveSubkey(newMaster, SyncCrypto::kContextWrap);
            SyncCrypto::wipe(newMaster);

            // Only the wrap changes - every itemsKey keeps its bytes, so
            // every noteKey and every already-synced record stays valid.
            // contentKey is not derived again either: it is a per-device
            // secret (adoptContentKey), and replacing it would make every
            // stored keyed hash stale and re-push the whole vault.
            QList<QPair<QByteArray, QByteArray>> rewrapped;
            for (const SyncJournal::ItemsKeyRecord &key : m_journal->allItemsKeys()) {
                const QByteArray wrap = SyncCrypto::seal(newWrapKey, key.plaintext,
                                                         SyncCrypto::buildAD("WK", {key.itemsKeyID, kWrapGeneration0}));
                rewrapped.append({key.itemsKeyID, wrap});
            }
            if (!m_client->changePassword(m_vaultID, newSalt, newLoginKey, rewrapped)) {
                setStatus(Status::Error, QStringLiteral("password change rejected by the server"));
                return;
            }
            m_journal->setMeta(QStringLiteral("salt"), newSalt);
            SyncCrypto::wipe(m_loginKey);
            SyncCrypto::wipe(m_wrapKey);
            m_loginKey = newLoginKey;
            m_wrapKey = newWrapKey;
            if (m_keysFromStore)
                storeKeys(); // the keyring's copy is the old password's keys now
            setStatus(Status::Idle, QStringLiteral("password changed"));
        },
        Qt::QueuedConnection);
}

void SyncEngine::queueLocalChange(const SyncScanner::Change &change)
{
    // The scanner can see names no other device may be asked to write (a
    // backslash, a dot folder, a link out of the vault): not pushed, named.
    if (!m_vault.isConfinedPath(change.path)) {
        if (!m_refusedPaths.contains(change.path))
            m_refusedPaths.append(change.path);
        if (change.kind == SyncScanner::Change::Deleted)
            m_journal->removeEntity(change.entityID);
        return;
    }
    const auto activeKey = m_journal->activeItemsKey();
    if (!activeKey)
        return;

    QByteArray entityID = change.entityID;
    QByteArray itemsKeyID = activeKey->itemsKeyID;
    QByteArray wrappedNoteKey;
    QByteArray noteKey;
    QByteArray syncedHash; // carried over from the existing entity, if any - see the field's own comment
    QByteArray baseDigest; // likewise: only applyRecord() moves it
    QByteArray parent;     // the state this record builds on: the entity's headDigest, none for a new entity

    if (change.kind == SyncScanner::Change::Created) {
        entityID = SyncCrypto::randomID();
        noteKey = SyncCrypto::randomKey();
        wrappedNoteKey =
            SyncCrypto::seal(activeKey->plaintext, noteKey, SyncCrypto::buildAD("IK", {entityID, itemsKeyID}));
    } else {
        const auto existing = m_journal->entity(entityID);
        if (!existing)
            return; // scanner referenced an entity this journal has never seen - next scan recovers
        itemsKeyID = existing->itemsKeyID;
        wrappedNoteKey = existing->wrappedNoteKey;
        syncedHash = existing->syncedHash;
        baseDigest = existing->baseDigest;
        parent = existing->headDigest;
        const auto key = itemsKeyID == activeKey->itemsKeyID ? activeKey : m_journal->itemsKey(itemsKeyID);
        if (!key)
            return;
        bool ok = false;
        noteKey =
            SyncCrypto::open(key->plaintext, wrappedNoteKey, SyncCrypto::buildAD("IK", {entityID, itemsKeyID}), &ok);
        if (!ok)
            return;
    }

    EntityPlain plain;
    plain.entityType = change.entityType;
    plain.path = change.path;
    plain.mtime = change.mtime;
    plain.parent = parent;

    QString text; // note bodies only
    QByteArray bodyBytes;

    if (change.kind == SyncScanner::Change::Deleted) {
        plain.op = QStringLiteral("delete");
    } else {
        plain.op = QStringLiteral("upsert");
        QByteArray raw;
        if (change.entityType == QStringLiteral("note")) {
            Note note;
            if (!Vault::read(m_vault.absolute(change.path), note))
                return;
            text = note.text;
        } else {
            QFile f(m_vault.absolute(change.path));
            if (!f.open(QIODevice::ReadOnly))
                return;
            raw = f.readAll();
        }
        bodyBytes = change.entityType == QStringLiteral("note") ? text.toUtf8() : raw;
        plain.hash = SyncCrypto::digest(bodyBytes);
        if (change.entityType == QStringLiteral("note") && bodyBytes.size() < kInlineBodyLimit) {
            plain.body = text;
        } else {
            const QByteArray padded = SyncCrypto::pad(bodyBytes);
            // entityID is in the preimage because the blob is sealed under this
            // entity's noteKey: two entities with equal bytes were one blob
            // under one noteKey, and the second could not open it.
            const QByteArray address = SyncCrypto::contentAddress(m_contentKey, entityID + padded);
            const QByteArray framed = SyncCrypto::sealBlob(noteKey, SyncCrypto::buildAD("BL", {address}), padded);
            if (framed.isEmpty() || !m_client->putBlob(m_vaultID, address, framed))
                return;
            plain.blobAddress = address;
        }
    }

    const QByteArray recordID = SyncCrypto::randomID();
    const QByteArray deviceID = m_journal->deviceID();
    const QByteArray payload = SyncCrypto::seal(
        noteKey, serialize(plain), SyncCrypto::buildAD("EN", {entityID, deviceID, recordID, itemsKeyID}));

    SyncJournal::OutboxRecord record;
    record.entityID = entityID;
    record.recordID = recordID;
    record.itemsKeyID = itemsKeyID;
    record.wrappedNoteKey = wrappedNoteKey;
    record.payload = payload;
    record.digest = plain.hash;
    m_journal->enqueue(record);

    if (change.kind == SyncScanner::Change::Deleted) {
        m_journal->removeEntity(entityID);
        return;
    }

    SyncJournal::Entity entity;
    entity.entityID = entityID;
    entity.path = change.path;
    entity.entityType = change.entityType;
    entity.itemsKeyID = itemsKeyID;
    entity.wrappedNoteKey = wrappedNoteKey;
    entity.localMtime = change.mtime;
    entity.localSize = change.size;
    entity.localHash = change.hash; // scanner dedup only
    entity.syncedHash = syncedHash; // unchanged by a local edit - see the field's own comment in SyncJournal.h
    entity.deleted = false;
    entity.baseDigest = baseDigest;
    entity.headDigest = plain.hash; // the next record's parent: not baseDigest, or two scans pushed in one cycle would both claim the old base
    m_journal->putEntity(entity);
    m_journal->addState(entityID, {plain.hash, SyncCrypto::contentAddress(m_contentKey, bodyBytes), change.path, text,
                                   true, 0});
    // Deliberately no writeBase() here: the base (and syncedHash above)
    // record what this device and the server last agreed on, and a local
    // edit that has not even been pushed yet has not agreed on anything.
    // applyRecord() advances both once a push is confirmed - including a
    // self-echo of this very push, which is how the common single-device
    // case still ends up with an up-to-date base without ever touching it
    // here.
}

int SyncEngine::pushOutbox()
{
    const QList<SyncJournal::OutboxRecord> pending = m_journal->pendingOutbox(500);
    if (pending.isEmpty())
        return 0;
    const QByteArray deviceID = m_journal->deviceID();
    QList<SyncClient::JournalRecord> wire;
    for (const SyncJournal::OutboxRecord &r : pending) {
        SyncClient::JournalRecord w;
        w.entityID = r.entityID;
        w.recordID = r.recordID;
        w.deviceID = deviceID;
        w.itemsKeyID = r.itemsKeyID;
        w.wrappedNoteKey = r.wrappedNoteKey;
        w.payload = r.payload;
        wire.append(w);
    }
    const SyncClient::PushResult result = m_client->pushJournal(m_vaultID, wire);
    if (!result.ok)
        return 0; // stays in the outbox, retried next cycle
    QList<QByteArray> ids;
    for (const SyncJournal::OutboxRecord &r : pending)
        ids << r.recordID;
    m_journal->markSent(ids);
    // The server's seq orders two devices' edits when they cannot be merged.
    if (result.seqs.size() == pending.size()) {
        for (int i = 0; i < pending.size(); ++i) {
            if (!pending.at(i).digest.isEmpty())
                m_journal->setStateSeq(pending.at(i).entityID, pending.at(i).digest, result.seqs.at(i));
        }
    }
    return pending.size();
}

QString SyncEngine::pullJournal(int *pulled, int *conflicts)
{
    for (;;) {
        const SyncClient::PullResult result = m_client->pullJournal(m_vaultID, m_journal->lastPulledSeq(), 500);
        if (!result.ok || result.records.isEmpty())
            break;
        for (const SyncClient::JournalRecord &record : result.records) {
            // Stop short of a record that could not be applied: the cursor
            // stays before it, so the next cycle tries it again, and nothing
            // after it is applied out of order.
            QString error;
            if (!applyRecord(record, pulled, conflicts, &error))
                return error;
            m_journal->setLastPulledSeq(record.seq);
        }
        if (result.records.size() < 500)
            break;
    }
    return {};
}

bool SyncEngine::applyRemoteDelete(const QByteArray &entityID, const QByteArray &parent, QString *error)
{
    const auto existing = m_journal->entity(entityID);
    // The deleter had not seen a state this device has since applied: the
    // delete is stale. Nothing is touched - the deleter gets that state as an
    // upsert in its next pull and re-creates the file (an edit beats a delete).
    if (existing && !parent.isEmpty() && !existing->baseDigest.isEmpty() && parent != existing->baseDigest)
        return true;
    const QString existingPath = existing ? m_vault.absolute(existing->path) : QString();
    QByteArray localBytes;
    bool hasLocalFile = false;
    if (existing) {
        QFile f(existingPath);
        hasLocalFile = f.open(QIODevice::ReadOnly);
        if (hasLocalFile)
            localBytes = f.readAll();
    }
    // An edit beats a delete, in both directions (docs/SYNC.md): only
    // remove the file when nothing local disagrees with the delete.
    const bool localChanged =
        existing && (!hasLocalFile || SyncCrypto::contentAddress(m_contentKey, localBytes) != existing->syncedHash);
    if (existing && hasLocalFile && localChanged) {
        // The entity stays and says so again: the edit goes out as that
        // entity's next record, so every device that dropped it gets it back.
        announce(entityID);
        return true;
    }
    // The entity goes only once the file has: dropped with the file
    // still there, the next scan would push it back as a new note and
    // undo the delete on every device.
    QString trashError;
    if (!localChanged && hasLocalFile && !m_vault.moveToTrash(existingPath, &trashError)) {
        *error = QStringLiteral("could not move %1 to the trash (%2); its deletion is retried next sync")
                     .arg(existing->path, trashError);
        return false;
    }
    m_journal->removeEntity(entityID);
    return true;
}

void SyncEngine::loadPendingDeletes()
{
    m_pendingDeletes.clear();
    for (const QByteArray &hex : m_journal->meta(QStringLiteral("pendingDeletes")).split('\n')) {
        if (!hex.isEmpty())
            m_pendingDeletes.append(QByteArray::fromHex(hex));
    }
}

void SyncEngine::setPendingDeletes(const QList<QByteArray> &ids)
{
    QByteArray joined;
    for (const QByteArray &id : ids)
        joined += (joined.isEmpty() ? "" : "\n") + id.toHex();
    m_pendingDeletes = ids;
    m_journal->setMeta(QStringLiteral("pendingDeletes"), joined);
}

void SyncEngine::retryPendingDeletes()
{
    QList<QByteArray> pending = pendingDeletes();
    for (int i = pending.size() - 1; i >= 0; --i) {
        QString error;
        if (applyRemoteDelete(pending.at(i), {}, &error))
            pending.removeAt(i);
    }
    if (pending.size() != pendingDeletes().size())
        setPendingDeletes(pending);
}

QString SyncEngine::pendingDeleteWarning() const
{
    QStringList names;
    for (const QByteArray &id : pendingDeletes()) {
        if (const auto entity = m_journal->entity(id))
            names << entity->path;
    }
    if (names.isEmpty())
        return {};
    return QStringLiteral("%1 deletion(s) from other devices could not be applied: %2; retried each sync")
        .arg(names.size())
        .arg(names.join(QStringLiteral(", ")));
}

QString SyncEngine::refusedWarning() const
{
    if (m_refusedPaths.isEmpty())
        return {};
    return QStringLiteral("%1 record(s) refused: paths outside the vault or with odd names: %2")
        .arg(m_refusedPaths.size())
        .arg(m_refusedPaths.mid(0, 5).join(QStringLiteral(", ")));
}

bool SyncEngine::loadRemoteContent(const QByteArray &recordID, const QString &path, const QByteArray &blobAddress,
                                   bool isNote, const QByteArray &noteKey, QString *text, QByteArray *raw,
                                   QString *error)
{
    if (blobAddress.isEmpty())
        return true; // inline body: *text already holds it
    const auto park = [&] {
        m_fetchStalls.remove(recordID);
        if (!m_unreadable.contains(path))
            m_unreadable.append(path);
        return false; // with *error empty: dropped, the pull moves on
    };
    const SyncClient::BlobResult blob = m_client->getBlob(m_vaultID, blobAddress);
    if (!blob.ok) {
        // Not dropped on the first failure (a blip must not lose an
        // attachment for good), and not allowed to hold every later record
        // forever either: kFetchStalls blocked cycles, then it is parked.
        if (++m_fetchStalls[recordID] > kFetchStalls)
            return park();
        *error = QStringLiteral("attachment %1 could not be fetched; retried next sync").arg(path);
        return false;
    }
    m_fetchStalls.remove(recordID);
    bool blobOk = false;
    const QByteArray padded =
        SyncCrypto::openBlob(noteKey, SyncCrypto::buildAD("BL", {blobAddress}), blob.bytes, &blobOk);
    if (!blobOk)
        return park(); // sealed under a key this device cannot use: retrying cannot help
    const QByteArray unpadded = SyncCrypto::unpad(padded, padded.size());
    if (isNote)
        *text = QString::fromUtf8(unpadded);
    else
        *raw = unpadded;
    return true;
}

QString SyncEngine::unreadableWarning() const
{
    if (m_unreadable.isEmpty())
        return {};
    return QStringLiteral("%1 attachment(s) from other devices could not be read: %2")
        .arg(m_unreadable.size())
        .arg(m_unreadable.mid(0, 5).join(QStringLiteral(", ")));
}

bool SyncEngine::applyRecord(const SyncClient::JournalRecord &record, int *pulled, int *conflicts, QString *error)
{
    const auto key = m_journal->itemsKey(record.itemsKeyID);
    if (!key)
        return true; // this device does not hold that itemsKey yet - enrolment will pick it up
    bool ok = false;
    const QByteArray noteKey = SyncCrypto::open(
        key->plaintext, record.wrappedNoteKey, SyncCrypto::buildAD("IK", {record.entityID, record.itemsKeyID}), &ok);
    if (!ok)
        return true;
    const QByteArray plaintext = SyncCrypto::open(
        noteKey, record.payload,
        SyncCrypto::buildAD("EN", {record.entityID, record.deviceID, record.recordID, record.itemsKeyID}), &ok);
    if (!ok)
        return true; // tampered, swapped between entities, or replayed - dropped, never applied

    bool parseOk = false;
    EntityPlain remote = parse(plaintext, &parseOk);
    if (!parseOk)
        return true;

    const auto existing = m_journal->entity(record.entityID);
    const QString existingPath = existing ? m_vault.absolute(existing->path) : QString();
    // Every path below comes from a record, so before anything touches the
    // disk: one that leaves the vault (or whose entity's own file is now a
    // link, or lies behind one) can never be applied here. Dropped, the way
    // an undecryptable record is, so it cannot wedge the pull - but named.
    if (!m_vault.isConfinedPath(remote.path, true) || (existing && !m_vault.isConfinedPath(existing->path))) {
        if (!m_refusedPaths.contains(remote.path))
            m_refusedPaths.append(remote.path);
        return true;
    }
    const bool isSelfEcho = record.deviceID == m_journal->deviceID();

    if (remote.op != QStringLiteral("delete")) {
        // A newer record for the entity (an edit, a rename, a re-create)
        // wins over a delete still waiting to be applied: drop it.
        m_deleteFailures.remove(record.entityID);
        if (pendingDeletes().contains(record.entityID)) {
            QList<QByteArray> pending = pendingDeletes();
            pending.removeAll(record.entityID);
            setPendingDeletes(pending);
        }
    }

    if (remote.op == QStringLiteral("delete")) {
        if (isSelfEcho) {
            // queueLocalChange() already removed the entity when it
            // pushed this delete - nothing left to confirm.
            ++*pulled;
            return true;
        }
        if (pendingDeletes().contains(record.entityID)) { // already given up on blocking for this one
            ++*pulled;
            return true;
        }
        if (!applyRemoteDelete(record.entityID, remote.parent, error)) {
            // The same record failing cycle after cycle (a read-only file,
            // a trash that never works) must not hold every later record,
            // from every device, behind it forever. After kDeleteStalls
            // blocked cycles the pull moves on and the delete is retried
            // each cycle on its own (retryPendingDeletes); the entity stays,
            // so the scan still sees the file as synced and never pushes it
            // back as a new note.
            if (++m_deleteFailures[record.entityID] <= kDeleteStalls)
                return false;
            m_deleteFailures.remove(record.entityID);
            QList<QByteArray> pending = pendingDeletes();
            pending.append(record.entityID);
            setPendingDeletes(pending);
            return true;
        }
        m_deleteFailures.remove(record.entityID);
        ++*pulled;
        return true;
    }

    if (isSelfEcho) {
        // Confirms a push this device already made - never re-derive file
        // content from this record's payload or from the vault file: both
        // may be stale by now if an earlier record in this same pull batch
        // triggered a merge. The state this device wrote is in the journal.
        if (!existing) {
            ++*pulled; // queueLocalChange always creates the entity before pushing - nothing to confirm
            return true;
        }
        if (remote.hash.isEmpty()) {
            // A record queued before records carried a hash: the old way,
            // mark whatever is on disk right now as the synced state.
            QByteArray syncedHash;
            QByteArray diskDigest;
            bool hasLocalFile = false;
            if (existing->entityType == QStringLiteral("note")) {
                Note onDisk;
                hasLocalFile = Vault::read(existingPath, onDisk);
                if (hasLocalFile) {
                    syncedHash = SyncCrypto::contentAddress(m_contentKey, onDisk.text.toUtf8());
                    diskDigest = SyncCrypto::digest(onDisk.text.toUtf8());
                    m_journal->writeBase(record.entityID, onDisk.text);
                }
            } else {
                QFile f(existingPath);
                hasLocalFile = f.open(QIODevice::ReadOnly);
                if (hasLocalFile) {
                    const QByteArray bytes = f.readAll();
                    syncedHash = SyncCrypto::contentAddress(m_contentKey, bytes);
                    diskDigest = SyncCrypto::digest(bytes);
                }
            }
            if (hasLocalFile) {
                const QFileInfo info(existingPath);
                SyncJournal::Entity entity = *existing;
                entity.localMtime = info.lastModified().toMSecsSinceEpoch();
                entity.localSize = info.size();
                entity.localHash = syncedHash;
                entity.syncedHash = syncedHash;
                entity.baseDigest = diskDigest; // the base file now holds this: the digest must say so too
                m_journal->putEntity(entity);
            }
            ++*pulled;
            return true;
        }
        const auto own = m_journal->state(record.entityID, remote.hash);
        m_journal->setStateSeq(record.entityID, remote.hash, record.seq); // even when skipped below
        // Walks the chain orig -> T1 -> T2. A remote record applied in between
        // moved baseDigest elsewhere, so superseded own records are skipped:
        // their content is in the merge result that was announced.
        if (own && remote.parent == existing->baseDigest) {
            SyncJournal::Entity entity = *existing;
            entity.baseDigest = remote.hash;
            entity.syncedHash = own->keyedHash;
            m_journal->putEntity(entity);
            if (existing->entityType == QStringLiteral("note"))
                m_journal->writeBase(record.entityID, own->text);
        }
        ++*pulled;
        return true;
    }

    QByteArray localBytes;
    bool hasLocalFile = false;
    if (existing) {
        QFile f(existingPath);
        hasLocalFile = f.open(QIODevice::ReadOnly);
        if (hasLocalFile)
            localBytes = f.readAll();
    }
    const bool localChanged =
        existing && (!hasLocalFile || SyncCrypto::contentAddress(m_contentKey, localBytes) != existing->syncedHash);

    QString remoteText = remote.body;
    QByteArray remoteRaw;
    if (!loadRemoteContent(record.recordID, remote.path, remote.blobAddress, remote.entityType == QStringLiteral("note"),
                           noteKey, &remoteText, &remoteRaw, error))
        return error->isEmpty(); // an error: retry the record; none: parked, dropped
    const bool isNote = remote.entityType == QStringLiteral("note");
    const QByteArray remoteBytes = isNote ? remoteText.toUtf8() : remoteRaw;
    // Computed, not taken from remote.hash: that is only trusted to find a self-echo's own state.
    const QByteArray remoteDigest = SyncCrypto::digest(remoteBytes);

    // Which state of this entity the sender built on, against the one this
    // device last confirmed (docs/SYNC.md "Conflict rules"). An empty parent
    // (a new entity, an old client) or an empty base (an old journal) is taken
    // as "in line": the decision then rests on this device's own state alone.
    const QByteArray baseDigest = existing ? existing->baseDigest : QByteArray();
    const bool inLine = remote.parent.isEmpty() || baseDigest.isEmpty() || remote.parent == baseDigest;
    const QByteArray parentDigest = inLine ? baseDigest : remote.parent;

    // Whether anything - a file, a directory, a link, even a dangling one -
    // sits at `path`. isFile() would call a directory or a broken link free,
    // and a write there goes through a link to wherever it points.
    const auto occupied = [](const QString &path) {
        const QFileInfo info(path);
        return info.exists() || info.isSymLink();
    };

    // Whether the file at `path` already holds exactly the incoming content.
    const auto holdsRemote = [&](const QString &path) {
        if (QFileInfo(path).isSymLink())
            return false; // a link is not the file: it must not be written through, nor count as a match
        if (isNote) {
            Note onDisk;
            return Vault::read(path, onDisk) && onDisk.text == remoteText;
        }
        QFile f(path);
        return f.open(QIODevice::ReadOnly) && f.readAll() == remoteRaw;
    };

    // The sender's path is what this device had at the state the sender built
    // on, so the sender did not rename: this device's path stands - a local
    // rename, or a rename it held back (the rows record the path as applied).
    if (existing && m_journal->sawPath(record.entityID, parentDigest, remote.path))
        remote.path = existing->path;

    // A rename (or move) onto a path that already holds something this entity
    // does not own - another entity's note, a stray file, a directory, a link
    // - would have QFile::rename fail and the write below overwrite it. The rename
    // is not applied instead: the entity stays at its own path and takes the
    // incoming content there, so nothing is lost, no second copy is made and
    // the journal stays consistent (path, file and hashes all agree). It is
    // a conflict unless the target already holds that very content. Later
    // records of the entity keep landing on its own path, and the rename
    // takes effect on its own once the target is free.
    if (existing && existing->path != remote.path && occupied(m_vault.absolute(remote.path))) {
        if (!holdsRemote(m_vault.absolute(remote.path)))
            ++*conflicts;
        remote.path = existing->path;
    }

    QString targetPath = m_vault.absolute(remote.path);
    // A remote rename/move into a folder this device has never had may
    // name a directory that does not exist here yet - Vault::write() and
    // QFile::rename() do not create parent directories, so this does.
    // Covers the conflict-copy path below too: same directory as `remote.path`.
    QDir().mkpath(QFileInfo(targetPath).path());

    // A rename or move takes the local file along before anything is merged
    // or written, whether or not it has a local edit: the merge then happens
    // at the new path and the entity saved below agrees with the file. A move
    // that cannot be done leaves the note at its own path, like an occupied
    // target above.
    bool moved = false;
    if (existing && existing->path != remote.path && hasLocalFile) {
        moved = QFile::rename(existingPath, targetPath);
        if (!moved) {
            remote.path = existing->path;
            targetPath = existingPath;
        }
    }

    // No entity of this record's own, yet a file already sits at its path
    // (a parked delete's file the person has since edited, or the same path
    // created independently on two devices): the incoming text must not
    // replace it. Different content goes to a conflict copy and the local
    // file is left alone; the same content needs nothing written. Either way
    // no entity is created here: another one may own the path, and two
    // entities on one path would make the scan read one of them as deleted.
    if (!existing && occupied(targetPath)) {
        if (!holdsRemote(targetPath)) {
            const QString copy = conflictCopyPath(m_vault, remote.path, record.deviceID);
            if (isNote)
                Vault::write(copy, Note{remoteText, false, false});
            else
                writeRawFile(copy, remoteRaw);
            ++*conflicts;
            return true;
        }
        if (m_journal->entityForPath(remote.path)) {
            ++*pulled;
            return true;
        }
    }

    const auto writeRemote = [&](bool keepDiskFormat) {
        if (isNote) {
            Note note;
            note.text = remoteText;
            if (keepDiskFormat) {
                Note onDisk;
                Vault::read(targetPath, onDisk);
                note.crlf = onDisk.crlf;
                note.bom = onDisk.bom;
            }
            Vault::write(targetPath, note);
        } else {
            writeRawFile(targetPath, remoteRaw);
        }
    };

    bool conflicted = false;
    bool announceMerge = false;
    // Whether the sender's content is on disk and the base moves to it with
    // nothing more to do (a new entity, a fast-forward, or the same content).
    if (!existing || !hasLocalFile) {
        writeRemote(false); // no local file: an edit beats a local delete
    } else if (holdsRemote(targetPath)) {
        // Same content: nothing to write, only the path (handled above) and the confirmation below.
    } else {
        if (inLine && !localChanged) {
            writeRemote(false); // fast-forward
        } else {
            // What the merge is based on: this device's own sync point when the
            // sender saw it, else the state the sender names. A state this
            // device no longer holds, or an attachment, cannot be merged: both
            // sides changed something and nothing says what.
            QString mergeBase;
            bool canMerge = false;
            if (isNote && inLine) {
                canMerge = true;
                mergeBase = m_journal->readBase(record.entityID);
            } else if (isNote) {
                if (const auto ancestor = m_journal->state(record.entityID, remote.parent)) {
                    canMerge = true;
                    mergeBase = ancestor->text;
                }
            }
            if (canMerge) {
                Note onDisk;
                Vault::read(targetPath, onDisk);
                const SyncMerge::Result merged = SyncMerge::merge(mergeBase, onDisk.text, remoteText);
                if (merged.clean) {
                    Note note;
                    note.text = merged.merged;
                    note.crlf = onDisk.crlf;
                    note.bom = onDisk.bom;
                    Vault::write(targetPath, note);
                    // The merge holds something the sender never had: it goes out
                    // with the sender's text as its parent, once - unless this
                    // very text is a state this device already wrote or applied.
                    announceMerge =
                        merged.merged != remoteText
                        && (moved || !m_journal->state(record.entityID, SyncCrypto::digest(merged.merged.toUtf8())));
                } else {
                    conflicted = true;
                }
            } else {
                conflicted = true;
            }
        }
        if (conflicted) {
            // Two edits that cannot be reconciled: the one the server saw last
            // is kept in the file, the other becomes a copy beside it. Exactly
            // one copy for two devices - the device holding the newer text
            // writes it (an own edit not yet acknowledged is the newest).
            QByteArray diskBytes = localBytes;
            if (isNote) {
                Note onDisk;
                Vault::read(targetPath, onDisk);
                diskBytes = onDisk.text.toUtf8();
            }
            const auto diskState = m_journal->state(record.entityID, SyncCrypto::digest(diskBytes));
            const qint64 diskSeq = diskState ? diskState->seq : 0;
            const bool localWins = diskSeq == 0 || diskSeq > record.seq;
            if (localWins) {
                const QString copy = conflictCopyPath(m_vault, remote.path, record.deviceID);
                if (isNote)
                    Vault::write(copy, Note{remoteText, false, false});
                else
                    writeRawFile(copy, remoteRaw);
            } else {
                writeRemote(true);
            }
        }
    }

    // The base is always the sender's text, never this device's merge result:
    // a later record built on it then merges against the state the sender saw.
    QFile written(targetPath);
    QByteArray diskNow;
    if (written.open(QIODevice::ReadOnly))
        diskNow = written.readAll();
    SyncJournal::Entity entity;
    entity.entityID = record.entityID;
    entity.path = remote.path;
    entity.entityType = remote.entityType;
    entity.itemsKeyID = record.itemsKeyID;
    entity.wrappedNoteKey = record.wrappedNoteKey;
    const QFileInfo info(targetPath);
    entity.localMtime = info.lastModified().toMSecsSinceEpoch();
    entity.localSize = info.size();
    entity.localHash = SyncCrypto::contentAddress(m_contentKey, diskNow);
    entity.syncedHash = SyncCrypto::contentAddress(m_contentKey, remoteBytes);
    entity.deleted = false;
    entity.baseDigest = remoteDigest;
    entity.headDigest = remoteDigest;
    m_journal->putEntity(entity);
    if (isNote)
        m_journal->writeBase(record.entityID, remoteText);
    m_journal->addState(record.entityID, {remoteDigest, entity.syncedHash, entity.path, remoteText, false, record.seq});

    if (announceMerge)
        announce(record.entityID);

    if (conflicted)
        ++*conflicts;
    else
        ++*pulled;
    return true;
}

// The entity as it now is on disk, queued for the other devices: a merge result
// they have not seen. queueLocalChange() puts the entity's headDigest, which
// applyRecord() has just set to the incoming text, as the record's parent.
void SyncEngine::announce(const QByteArray &entityID)
{
    const auto entity = m_journal->entity(entityID);
    if (!entity)
        return;
    SyncScanner::Change change;
    change.kind = SyncScanner::Change::Modified;
    change.entityID = entityID;
    change.path = entity->path;
    change.entityType = entity->entityType;
    change.mtime = entity->localMtime;
    change.size = entity->localSize;
    change.hash = entity->localHash;
    queueLocalChange(change);
}

} // namespace noteAppC
