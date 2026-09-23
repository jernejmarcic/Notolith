#include "SyncEngine.h"

#include <algorithm>

#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <qt6keychain/keychain.h>

#include "SyncCrypto.h"
#include "SyncJournal.h"
#include "SyncMerge.h"

namespace noteAppC {

namespace {

constexpr qsizetype kInlineBodyLimit = 16 * 1024; ///< docs/SYNC.md "Record and blob formats"
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
};

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
/// call site each.
void runKeychainJob(QKeychain::Job *job)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
}

} // namespace

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
    m_running = false;
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
    m_client.reset();
    m_journal.reset();
    setStatus(Status::Idle);
}

void SyncEngine::runCycle()
{
    if (!m_running)
        return;
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
    pullJournal(&pulled, &conflicts);

    maintainEnrolment();
    setStatus(Status::Idle);
    emit syncFinished(pushed, pulled, conflicts);
}

bool SyncEngine::ensureSession()
{
    if (!m_client->token().isEmpty() && !m_vaultID.isEmpty())
        return true;

    QByteArray salt = m_journal->meta(QStringLiteral("salt"));
    if (salt.isEmpty()) {
        const SyncClient::Salt fetched = m_client->fetchSalt(m_email);
        if (!fetched.ok) {
            setStatus(Status::Error, QStringLiteral("could not reach the server"));
            return false;
        }
        salt = fetched.salt;
    }

    if (m_wrapKey.isEmpty()) {
        const QString password = m_prompt ? m_prompt(m_email) : QString();
        if (password.isEmpty()) {
            setStatus(Status::Error, QStringLiteral("no password given"));
            return false;
        }
        QByteArray masterKey = SyncCrypto::deriveMasterKey(password.toUtf8(), salt);
        if (masterKey.isEmpty()) {
            setStatus(Status::Error, QStringLiteral("key derivation failed"));
            return false;
        }
        m_loginKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextLogin);
        m_wrapKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextWrap);
        m_contentKey = SyncCrypto::deriveSubkey(masterKey, SyncCrypto::kContextContent);
        SyncCrypto::wipe(masterKey);
    }

    SyncClient::Auth auth = m_client->signIn(m_email, m_loginKey);
    if (!auth.ok)
        auth = m_client->registerAccount(m_email, salt, m_loginKey);
    if (!auth.ok) {
        setStatus(Status::Error, auth.error.isEmpty() ? QStringLiteral("sign-in failed") : auth.error);
        return false;
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

    const QString service = QStringLiteral("noteAppC-sync");
    const QString cachedPublic = QString::fromLatin1(m_journal->meta(QStringLiteral("boxPublicKey")).toHex());
    const QString keychainKey = QStringLiteral("device-box-secret-") + QString::fromLatin1(m_journal->deviceID().toHex());

    if (!cachedPublic.isEmpty()) {
        QKeychain::ReadPasswordJob read(service);
        read.setAutoDelete(false);
        read.setKey(keychainKey);
        runKeychainJob(&read);
        if (read.error() == QKeychain::NoError) {
            m_boxPublicKey = m_journal->meta(QStringLiteral("boxPublicKey"));
            m_boxSecretKey = read.binaryData();
            return true;
        }
    }

    // Nothing usable in the keyring yet: mint a fresh device identity.
    const SyncCrypto::BoxKeyPair pair = SyncCrypto::generateBoxKeyPair();
    m_boxPublicKey = pair.publicKey;
    m_boxSecretKey = pair.secretKey;
    m_journal->setMeta(QStringLiteral("boxPublicKey"), m_boxPublicKey);

    QKeychain::WritePasswordJob write(service);
    write.setAutoDelete(false);
    write.setKey(keychainKey);
    write.setBinaryData(m_boxSecretKey);
    runKeychainJob(&write);
    return write.error() == QKeychain::NoError;
}

bool SyncEngine::ensureItemsKey()
{
    if (m_journal->activeItemsKey())
        return true;
    if (!loadDeviceIdentity())
        return false;

    const QByteArray deviceID = m_journal->deviceID();
    m_client->registerDevice(m_vaultID, deviceID, m_boxPublicKey); // idempotent

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
        setStatus(Status::WaitingForApproval, QStringLiteral("waiting for an existing device to approve this one"));
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

    for (const SyncClient::DeviceInfo &device : m_client->listDevices(m_vaultID)) {
        if (device.deviceID == deviceID)
            continue;
        const QList<SyncClient::Lockbox> already = m_client->listLockboxes(m_vaultID, device.deviceID);
        for (const SyncJournal::ItemsKeyRecord &key : m_journal->allItemsKeys()) {
            const bool have = std::any_of(already.cbegin(), already.cend(),
                                          [&key](const SyncClient::Lockbox &box) { return box.itemsKeyID == key.itemsKeyID; });
            if (have)
                continue;
            const QByteArray sealed = SyncCrypto::sealToBox(device.publicKey, key.plaintext);
            m_client->putLockbox(m_vaultID, device.deviceID, key.itemsKeyID, sealed);
        }
    }
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
            const QByteArray newContentKey = SyncCrypto::deriveSubkey(newMaster, SyncCrypto::kContextContent);
            SyncCrypto::wipe(newMaster);

            // Only the wrap changes - every itemsKey keeps its bytes, so
            // every noteKey and every already-synced record stays valid.
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
            SyncCrypto::wipe(m_contentKey);
            m_loginKey = newLoginKey;
            m_wrapKey = newWrapKey;
            m_contentKey = newContentKey;
            setStatus(Status::Idle, QStringLiteral("password changed"));
        },
        Qt::QueuedConnection);
}

void SyncEngine::queueLocalChange(const SyncScanner::Change &change)
{
    const auto activeKey = m_journal->activeItemsKey();
    if (!activeKey)
        return;

    QByteArray entityID = change.entityID;
    QByteArray itemsKeyID = activeKey->itemsKeyID;
    QByteArray wrappedNoteKey;
    QByteArray noteKey;

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

    QString text; // note bodies only - also becomes the new merge base below

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
        const QByteArray bodyBytes = change.entityType == QStringLiteral("note") ? text.toUtf8() : raw;
        if (change.entityType == QStringLiteral("note") && bodyBytes.size() < kInlineBodyLimit) {
            plain.body = text;
        } else {
            const QByteArray padded = SyncCrypto::pad(bodyBytes);
            const QByteArray address = SyncCrypto::contentAddress(m_contentKey, padded);
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
    entity.localHash = change.hash;
    entity.deleted = false;
    m_journal->putEntity(entity);
    if (change.entityType == QStringLiteral("note"))
        m_journal->writeBase(entityID, text);
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
    return pending.size();
}

void SyncEngine::pullJournal(int *pulled, int *conflicts)
{
    for (;;) {
        const SyncClient::PullResult result = m_client->pullJournal(m_vaultID, m_journal->lastPulledSeq(), 500);
        if (!result.ok || result.records.isEmpty())
            break;
        for (const SyncClient::JournalRecord &record : result.records) {
            applyRecord(record, pulled, conflicts);
            m_journal->setLastPulledSeq(record.seq);
        }
        if (result.records.size() < 500)
            break;
    }
}

void SyncEngine::applyRecord(const SyncClient::JournalRecord &record, int *pulled, int *conflicts)
{
    const auto key = m_journal->itemsKey(record.itemsKeyID);
    if (!key)
        return; // this device does not hold that itemsKey yet - enrolment will pick it up
    bool ok = false;
    const QByteArray noteKey = SyncCrypto::open(
        key->plaintext, record.wrappedNoteKey, SyncCrypto::buildAD("IK", {record.entityID, record.itemsKeyID}), &ok);
    if (!ok)
        return;
    const QByteArray plaintext = SyncCrypto::open(
        noteKey, record.payload,
        SyncCrypto::buildAD("EN", {record.entityID, record.deviceID, record.recordID, record.itemsKeyID}), &ok);
    if (!ok)
        return; // tampered, swapped between entities, or replayed - dropped, never applied

    bool parseOk = false;
    const EntityPlain remote = parse(plaintext, &parseOk);
    if (!parseOk)
        return;

    const auto existing = m_journal->entity(record.entityID);
    const QString existingPath = existing ? m_vault.absolute(existing->path) : QString();

    QByteArray localBytes;
    bool hasLocalFile = false;
    if (existing) {
        QFile f(existingPath);
        hasLocalFile = f.open(QIODevice::ReadOnly);
        if (hasLocalFile)
            localBytes = f.readAll();
    }
    const bool localChanged =
        existing && (!hasLocalFile || SyncCrypto::contentAddress(m_contentKey, localBytes) != existing->localHash);

    if (remote.op == QStringLiteral("delete")) {
        // An edit beats a delete, in both directions (docs/SYNC.md): only
        // remove the file when nothing local disagrees with the delete.
        if (!localChanged && hasLocalFile)
            m_vault.moveToTrash(existingPath);
        m_journal->removeEntity(record.entityID);
        ++*pulled;
        return;
    }

    QString remoteText = remote.body;
    QByteArray remoteRaw;
    if (!remote.blobAddress.isEmpty()) {
        const SyncClient::BlobResult blob = m_client->getBlob(m_vaultID, remote.blobAddress);
        if (!blob.ok)
            return;
        bool blobOk = false;
        const QByteArray padded =
            SyncCrypto::openBlob(noteKey, SyncCrypto::buildAD("BL", {remote.blobAddress}), blob.bytes, &blobOk);
        if (!blobOk)
            return;
        const QByteArray unpadded = SyncCrypto::unpad(padded, padded.size());
        if (remote.entityType == QStringLiteral("note"))
            remoteText = QString::fromUtf8(unpadded);
        else
            remoteRaw = unpadded;
    }

    const QString targetPath = m_vault.absolute(remote.path);
    bool wroteConflict = false;

    if (!localChanged) {
        if (existing && existing->path != remote.path && hasLocalFile)
            QFile::rename(existingPath, targetPath);
        if (remote.entityType == QStringLiteral("note")) {
            Note note;
            note.text = remoteText;
            Vault::write(targetPath, note);
        } else {
            writeRawFile(targetPath, remoteRaw);
        }
    } else if (remote.entityType == QStringLiteral("note")) {
        const QString base = m_journal->readBase(record.entityID);
        Note onDisk;
        Vault::read(existingPath, onDisk);
        const SyncMerge::Result merged = SyncMerge::merge(base, onDisk.text, remoteText);
        if (merged.clean) {
            Note note;
            note.text = merged.merged;
            note.crlf = onDisk.crlf;
            note.bom = onDisk.bom;
            Vault::write(existingPath, note);
            remoteText = merged.merged; // the new base - and what the next scan will find on disk
        } else {
            Vault::write(conflictCopyPath(m_vault, remote.path, record.deviceID), Note{remoteText, false, false});
            wroteConflict = true;
            remoteText = onDisk.text; // local is left exactly as it is
        }
    } else {
        // Two sides touched the same attachment: never silently pick one.
        writeRawFile(conflictCopyPath(m_vault, remote.path, record.deviceID), remoteRaw);
        wroteConflict = true;
    }

    const QString finalPath = wroteConflict && existing ? existingPath : targetPath;
    const QByteArray finalBytes = wroteConflict && existing
                                    ? localBytes
                                    : (remote.entityType == QStringLiteral("note") ? remoteText.toUtf8() : remoteRaw);

    SyncJournal::Entity entity;
    entity.entityID = record.entityID;
    entity.path = wroteConflict && existing ? existing->path : remote.path;
    entity.entityType = remote.entityType;
    entity.itemsKeyID = record.itemsKeyID;
    entity.wrappedNoteKey = record.wrappedNoteKey;
    const QFileInfo info(finalPath);
    entity.localMtime = info.lastModified().toMSecsSinceEpoch();
    entity.localSize = info.size();
    entity.localHash = SyncCrypto::contentAddress(m_contentKey, finalBytes);
    entity.deleted = false;
    m_journal->putEntity(entity);
    if (remote.entityType == QStringLiteral("note"))
        m_journal->writeBase(record.entityID, remoteText);

    if (wroteConflict)
        ++*conflicts;
    else
        ++*pulled;
}

} // namespace noteAppC
