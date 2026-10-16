// Sync (libs/ext/sync, tools/sync_server): crypto round trips and tamper
// detection, the three-way merge, and full two-device sync scenarios
// against a real in-process server. See docs/SYNC.md for what every
// scenario here is checking against.
//
// Every test gets its own $XDG_DATA_HOME (so nothing reaches the real
// ~/.local/share) and its own vault directories. Device enrolment tests do
// write a handful of entries to the *real* OS keyring under the service
// name "noteAppC-sync" (SyncEngine's device identity key -
// libs/ext/sync/SyncEngine.cpp's loadDeviceIdentity()) - not cleaned up
// automatically here; harmless (a random X25519 keypair, not a real
// secret) but worth knowing if this suite has run on a machine before.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <unistd.h>

#include <QTest>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QMutex>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "ServerStore.h"
#include "SyncClient.h"
#include "SyncCrypto.h"
#include "SyncEngine.h"
#include "SyncJournal.h"
#include "SyncMerge.h"
#include "SyncScanner.h"
#include "SyncServer.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

QString writeNote(const QString &vaultRoot, const QString &relativePath, const QString &text)
{
    const QString absolute = vaultRoot + QLatin1Char('/') + relativePath;
    QDir().mkpath(QFileInfo(absolute).path());
    Note note;
    note.text = text;
    EXPECT_TRUE(Vault::write(absolute, note));
    return absolute;
}

QString readNoteText(const QString &absolutePath)
{
    Note note;
    if (!Vault::read(absolutePath, note))
        return {};
    return note.text;
}

void writeRaw(const QString &absolutePath, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(absolutePath).path());
    QFile f(absolutePath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

/// Every file under `root`, recursively, as raw bytes plus its path -
/// what the no-plaintext check scans.
QList<QPair<QString, QByteArray>> allFilesUnder(const QString &root)
{
    QList<QPair<QString, QByteArray>> out;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        out.append({path, f.readAll()});
    }
    return out;
}

SyncEngine::PasswordPrompt fixedPassword(const QString &password)
{
    return [password](const QString &) { return password; };
}

/// The process-wide in-memory secret store main() installs - the engines'
/// stand-in for the OS keyring. Tests look inside it for the keys entry.
QMutex g_secretsMutex;
QHash<QString, QByteArray> g_secrets;

QStringList keyEntries()
{
    const QMutexLocker lock(&g_secretsMutex);
    QStringList out;
    for (auto it = g_secrets.cbegin(); it != g_secrets.cend(); ++it)
        if (it.key().startsWith(QStringLiteral("keys/")))
            out.append(it.key());
    return out;
}

void clearKeyEntries()
{
    for (const QString &name : keyEntries()) {
        const QMutexLocker lock(&g_secretsMutex);
        g_secrets.remove(name);
    }
}

/// A prompt that answers `password` and counts how often it was asked.
SyncEngine::PasswordPrompt countingPrompt(const QString &password, const std::shared_ptr<std::atomic<int>> &asked)
{
    return [password, asked](const QString &) {
        ++*asked;
        return password;
    };
}

/// Tracks a SyncEngine's signals so a test can wait on a cycle finishing
/// and inspect the counts/status afterwards - SyncEngine's own API is
/// deliberately just start/stop/noteChanged/syncNow and two signals (see
/// SyncEngine.h), so every test drives it through exactly that surface.
///
/// A QObject, and not just for the connection-lifetime bookkeeping: a
/// `connect(sender, signal, functor)` with *no* context object makes Qt
/// invoke the functor directly on whichever thread does the emitting
/// (there is no receiver to decide a thread for), documented behaviour,
/// not a bug in Qt. SyncEngine emits from its own worker thread, so a
/// bare-lambda version of this struct had its fields written from that
/// thread while the test's main thread concurrently read them in
/// waitForCycle()'s poll loop below - an unsynchronised data race on a
/// QString (its refcount is not atomic-safe against that) that showed up,
/// three tests into a run, as a segfault inside unrelated-looking
/// QSqlQuery code: the race corrupted heap state whose corruption only
/// became visible once something else's allocation reused it. Passing
/// `this` as the connection's context restores the intended behaviour -
/// Qt then sees sender and context on different threads and queues the
/// call onto the context's (this struct's, i.e. the main thread's) event
/// loop instead, which waitForCycle()'s own loop.exec() calls pump.
struct Watcher : public QObject {
    SyncEngine::Status lastStatus = SyncEngine::Status::Idle;
    QString lastMessage;
    int pushed = 0;
    int pulled = 0;
    int conflicts = 0;
    int finishedCount = 0;

    explicit Watcher(SyncEngine &engine)
    {
        QObject::connect(&engine, &SyncEngine::statusChanged, this, [this](SyncEngine::Status s, const QString &m) {
            lastStatus = s;
            lastMessage = m;
        });
        QObject::connect(&engine, &SyncEngine::syncFinished, this, [this](int p, int pl, int c) {
            pushed = p;
            pulled = pl;
            conflicts = c;
            ++finishedCount;
        });
    }
    Watcher(const Watcher &) = delete;
    Watcher &operator=(const Watcher &) = delete;

    /// Pumps the calling (main) thread's event loop in short slices until
    /// one more syncFinished has landed or `timeoutMs` has elapsed - short
    /// slices rather than one long QEventLoop::exec() so a signal that
    /// fires between "check the counter" and "start waiting" is never
    /// missed.
    bool waitForCycle(int timeoutMs = 40000)
    {
        const int target = finishedCount + 1;
        QElapsedTimer clock;
        clock.start();
        while (finishedCount < target && clock.elapsed() < timeoutMs) {
            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return finishedCount >= target;
    }

    /// For enrolment polling: pumps the loop for a short slice without
    /// requiring a syncFinished (WaitingForApproval never emits one).
    void pump(int ms = 200)
    {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    }

    /// Polls until lastStatus reaches `status` or `timeoutMs` elapses -
    /// used for WaitingForApproval, which (unlike a finished cycle) has no
    /// signal of its own to key a single wait off. Short slices for the
    /// same reason waitForCycle() uses them.
    bool waitForStatus(SyncEngine::Status status, int timeoutMs = 40000)
    {
        QElapsedTimer clock;
        clock.start();
        while (lastStatus != status && clock.elapsed() < timeoutMs)
            pump(50);
        return lastStatus == status;
    }
};

/// A real server (the actual ServerStore/SyncServer code, not a fake) on
/// loopback, in this process - see tests/CMakeLists.txt's comment on why
/// test_sync compiles tools/sync_server's two files in directly.
class SyncIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dataHome.isValid());
        qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
        ASSERT_TRUE(serverData.isValid());
        ASSERT_TRUE(vaultA.isValid());
        ASSERT_TRUE(vaultB.isValid());
        store = std::make_unique<ServerStore>(serverData.path());
        ASSERT_TRUE(store->isOpen()) << store->error().toStdString();
        server = std::make_unique<SyncServer>(store.get());
        ASSERT_TRUE(server->listen(QStringLiteral("127.0.0.1"), 0));
        serverURL = QStringLiteral("http://127.0.0.1:%1").arg(server->actualPort());
        clearKeyEntries();
    }

    QTemporaryDir dataHome;
    QTemporaryDir serverData;
    QTemporaryDir vaultA;
    QTemporaryDir vaultB;
    std::unique_ptr<ServerStore> store;
    std::unique_ptr<SyncServer> server;
    QString serverURL;
    const QString email = QStringLiteral("jernej@example.test");
    const QString password = QStringLiteral("correct horse battery staple");
};

} // namespace

// ---------------------------------------------------------------------
// Crypto: round trips and tamper detection
// ---------------------------------------------------------------------

TEST(SyncCryptoTest, KDFisDeterministicAndSaltChangesIt)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray salt = SyncCrypto::randomSalt();
    const QByteArray a = SyncCrypto::deriveMasterKey("hunter2", salt);
    const QByteArray b = SyncCrypto::deriveMasterKey("hunter2", salt);
    ASSERT_EQ(a.size(), 32);
    EXPECT_EQ(a, b);
    const QByteArray differentSalt = SyncCrypto::deriveMasterKey("hunter2", SyncCrypto::randomSalt());
    EXPECT_NE(a, differentSalt);
    const QByteArray differentPassword = SyncCrypto::deriveMasterKey("hunter3", salt);
    EXPECT_NE(a, differentPassword);
}

TEST(SyncCryptoTest, SubkeysAreDistinctPerContext)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray master = SyncCrypto::randomKey();
    const QByteArray login = SyncCrypto::deriveSubkey(master, SyncCrypto::kContextLogin);
    const QByteArray wrap = SyncCrypto::deriveSubkey(master, SyncCrypto::kContextWrap);
    const QByteArray content = SyncCrypto::deriveSubkey(master, SyncCrypto::kContextContent);
    EXPECT_NE(login, wrap);
    EXPECT_NE(login, content);
    EXPECT_NE(wrap, content);
    // A one-way function: knowing loginKey must not hand back masterKey.
    EXPECT_NE(login, master);
}

TEST(SyncCryptoTest, AEADRoundTripsAndDetectsATamperedByte)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray key = SyncCrypto::randomKey();
    const QByteArray ad = SyncCrypto::buildAD("EN", {QByteArrayLiteral("entity1"), QByteArrayLiteral("device1")});
    const QByteArray plaintext = QByteArrayLiteral("the quick brown fox");
    const QByteArray sealed = SyncCrypto::seal(key, plaintext, ad);
    ASSERT_FALSE(sealed.isEmpty());

    bool ok = false;
    const QByteArray opened = SyncCrypto::open(key, sealed, ad, &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(opened, plaintext);

    QByteArray tampered = sealed;
    tampered[tampered.size() - 1] = char(tampered.at(tampered.size() - 1) ^ 0x01);
    ok = true; // must be flipped to false by open()
    const QByteArray rejected = SyncCrypto::open(key, tampered, ad, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(SyncCryptoTest, RecordsCannotBeSwappedBetweenEntities)
{
    // "swap records between notes" (the task's own wording): sealing under
    // entity A's AD, then trying to open as if it were entity B's, must
    // fail even with the correct key - AD binds identity into the tag.
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray key = SyncCrypto::randomKey();
    const QByteArray adForA = SyncCrypto::buildAD("EN", {QByteArrayLiteral("noteA")});
    const QByteArray adForB = SyncCrypto::buildAD("EN", {QByteArrayLiteral("noteB")});
    const QByteArray sealedForA = SyncCrypto::seal(key, QByteArrayLiteral("A's secret text"), adForA);

    bool ok = true;
    const QByteArray result = SyncCrypto::open(key, sealedForA, adForB, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(result.isEmpty());
}

TEST(SyncCryptoTest, PaddingHidesTheExactSizeButRoundTrips)
{
    ASSERT_TRUE(SyncCrypto::init());
    EXPECT_EQ(SyncCrypto::bucketFor(1), 1024);
    EXPECT_EQ(SyncCrypto::bucketFor(1024), 4096); // strictly less than the bucket - room for the pad byte
    EXPECT_EQ(SyncCrypto::bucketFor(1023), 1024);
    EXPECT_EQ(SyncCrypto::bucketFor(5000), 16384);

    for (const int size : {0, 1, 1023, 1024, 70000}) {
        const QByteArray plaintext(size, 'x');
        const QByteArray padded = SyncCrypto::pad(plaintext);
        EXPECT_EQ(padded.size(), SyncCrypto::bucketFor(size));
        const QByteArray unpadded = SyncCrypto::unpad(padded, padded.size());
        EXPECT_EQ(unpadded, plaintext);
    }
}

TEST(SyncCryptoTest, SealBlobOpenBlobRoundTripsAcrossChunkBoundary)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray key = SyncCrypto::randomKey();
    const QByteArray ad = SyncCrypto::buildAD("BL", {QByteArrayLiteral("addr")});

    // Small (well under one chunk) and large (several chunks) - the same
    // fixed-chunk framing has to round-trip both.
    for (const qint64 size : {qint64(0), qint64(10), SyncCrypto::kStreamChunk, SyncCrypto::kStreamChunk * 2 + 500}) {
        QByteArray plaintext(int(size), Qt::Uninitialized);
        for (qint64 i = 0; i < size; ++i)
            plaintext[int(i)] = char(i % 251);
        const QByteArray framed = SyncCrypto::sealBlob(key, ad, plaintext);
        ASSERT_FALSE(framed.isEmpty()) << "size " << size;
        bool ok = false;
        const QByteArray opened = SyncCrypto::openBlob(key, ad, framed, &ok);
        EXPECT_TRUE(ok) << "size " << size;
        EXPECT_EQ(opened, plaintext) << "size " << size;
    }
}

TEST(SyncCryptoTest, TruncatedBlobIsRejectedNotHandedBackPartial)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray key = SyncCrypto::randomKey();
    const QByteArray ad = SyncCrypto::buildAD("BL", {QByteArrayLiteral("addr")});
    const QByteArray plaintext(int(SyncCrypto::kStreamChunk) * 2, 'z');
    const QByteArray framed = SyncCrypto::sealBlob(key, ad, plaintext);
    ASSERT_GT(framed.size(), 100);

    const QByteArray truncated = framed.left(framed.size() - 50); // drop the final chunk's tail
    bool ok = true;
    const QByteArray result = SyncCrypto::openBlob(key, ad, truncated, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(result.isEmpty());
}

TEST(SyncCryptoTest, LockboxOpensOnlyWithTheMatchingSecretKey)
{
    ASSERT_TRUE(SyncCrypto::init());
    const SyncCrypto::BoxKeyPair pair = SyncCrypto::generateBoxKeyPair();
    const SyncCrypto::BoxKeyPair wrongPair = SyncCrypto::generateBoxKeyPair();
    const QByteArray sealed = SyncCrypto::sealToBox(pair.publicKey, QByteArrayLiteral("an itemsKey, 32 bytes long!!!!!"));

    bool ok = false;
    const QByteArray opened = SyncCrypto::openBox(pair, sealed, &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(opened, QByteArrayLiteral("an itemsKey, 32 bytes long!!!!!"));

    ok = true;
    const QByteArray rejected = SyncCrypto::openBox(wrongPair, sealed, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(SyncCryptoTest, AFingerprintIsShortStableAndPerKey)
{
    const auto one = SyncCrypto::generateBoxKeyPair();
    const auto two = SyncCrypto::generateBoxKeyPair();
    const QString code = SyncCrypto::fingerprint(one.publicKey);
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^([0-9A-F]{4}-){4}[0-9A-F]{4}$")).match(code).hasMatch()) << code.toStdString();
    EXPECT_EQ(code, SyncCrypto::fingerprint(one.publicKey));
    EXPECT_NE(code, SyncCrypto::fingerprint(two.publicKey));
}

TEST(SyncCryptoTest, LoginKeyHashRoundTripsAndRejectsTheWrongKey)
{
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray loginKey = SyncCrypto::randomKey();
    const QByteArray hash = SyncCrypto::hashLoginKey(loginKey);
    ASSERT_FALSE(hash.isEmpty());
    EXPECT_TRUE(SyncCrypto::verifyLoginKey(hash, loginKey));
    EXPECT_FALSE(SyncCrypto::verifyLoginKey(hash, SyncCrypto::randomKey()));
}

TEST(SyncCryptoTest, KeyDerivationTiming)
{
    // Not a pass/fail assertion on wall clock (a shared CI box is not a
    // benchmark), just a recorded number for the report - see the task's
    // request for a measured KDF time. Printed via GTest's own message so
    // it shows up in -output-on-failure and in a plain run alike.
    ASSERT_TRUE(SyncCrypto::init());
    const QByteArray salt = SyncCrypto::randomSalt();
    QElapsedTimer clock;
    clock.start();
    const QByteArray key = SyncCrypto::deriveMasterKey("a representative password", salt);
    const qint64 ms = clock.elapsed();
    ASSERT_FALSE(key.isEmpty());
    std::cout << "[ INFO     ] Argon2id (64 MiB, 5 ops, 1 lane): " << ms << " ms\n";
    EXPECT_LT(ms, 5000); // generous - just catches a parameter typo turning this into minutes
}

// ---------------------------------------------------------------------
// Three-way merge
// ---------------------------------------------------------------------

TEST(SyncMergeTest, OnlyOneSideChangedIsClean)
{
    const QString base = QStringLiteral("line1\nline2\nline3");
    const QString local = base; // unchanged locally
    const QString remote = QStringLiteral("line1\nline2 EDITED\nline3");
    const SyncMerge::Result result = SyncMerge::merge(base, local, remote);
    ASSERT_TRUE(result.clean);
    EXPECT_EQ(result.merged, remote);
}

TEST(SyncMergeTest, DisjointEditsMergeCleanly)
{
    const QString base = QStringLiteral("alpha\nbeta\ngamma\ndelta");
    const QString local = QStringLiteral("ALPHA\nbeta\ngamma\ndelta");   // local touched line 1
    const QString remote = QStringLiteral("alpha\nbeta\ngamma\nDELTA"); // remote touched line 4
    const SyncMerge::Result result = SyncMerge::merge(base, local, remote);
    ASSERT_TRUE(result.clean);
    EXPECT_EQ(result.merged, QStringLiteral("ALPHA\nbeta\ngamma\nDELTA"));
}

TEST(SyncMergeTest, SameLineEditedDifferentlyIsAConflict)
{
    const QString base = QStringLiteral("alpha\nbeta\ngamma");
    const QString local = QStringLiteral("alpha\nBETA-LOCAL\ngamma");
    const QString remote = QStringLiteral("alpha\nBETA-REMOTE\ngamma");
    const SyncMerge::Result result = SyncMerge::merge(base, local, remote);
    EXPECT_FALSE(result.clean);
}

TEST(SyncMergeTest, IdenticalEditsOnBothSidesAreNotAConflict)
{
    const QString base = QStringLiteral("alpha\nbeta");
    const QString local = QStringLiteral("alpha\nBETA");
    const QString remote = QStringLiteral("alpha\nBETA");
    const SyncMerge::Result result = SyncMerge::merge(base, local, remote);
    ASSERT_TRUE(result.clean);
    EXPECT_EQ(result.merged, local);
}

// ---------------------------------------------------------------------
// SyncScanner: mtime/size before hashing, content-based renames
// ---------------------------------------------------------------------

TEST(SyncScannerTest, NewNoteIsCreatedAndUnchangedNoteIsSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTemporaryDir dataHome;
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    const Vault vault(dir.path());
    SyncJournal journal(dir.path());
    ASSERT_TRUE(journal.isOpen());
    const QByteArray contentKey = SyncCrypto::randomKey();

    writeNote(dir.path(), QStringLiteral("note.md"), QStringLiteral("hello"));
    const QList<SyncScanner::Change> first = SyncScanner::scan(vault, journal, contentKey);
    ASSERT_EQ(first.size(), 1);
    EXPECT_EQ(first.first().kind, SyncScanner::Change::Created);

    // Record it as synced, then scan again with nothing touched.
    SyncJournal::Entity entity;
    entity.entityID = SyncCrypto::randomID();
    entity.path = first.first().path;
    entity.entityType = first.first().entityType;
    entity.localMtime = first.first().mtime;
    entity.localSize = first.first().size;
    entity.localHash = first.first().hash;
    journal.putEntity(entity);

    const QList<SyncScanner::Change> second = SyncScanner::scan(vault, journal, contentKey);
    EXPECT_TRUE(second.isEmpty());
}

TEST(SyncScannerTest, RenameIsDetectedByContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTemporaryDir dataHome;
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    const Vault vault(dir.path());
    SyncJournal journal(dir.path());
    const QByteArray contentKey = SyncCrypto::randomKey();

    const QString path = writeNote(dir.path(), QStringLiteral("old.md"), QStringLiteral("same content"));
    const QList<SyncScanner::Change> created = SyncScanner::scan(vault, journal, contentKey);
    ASSERT_EQ(created.size(), 1);
    SyncJournal::Entity entity;
    entity.entityID = SyncCrypto::randomID();
    entity.path = created.first().path;
    entity.entityType = created.first().entityType;
    entity.localMtime = created.first().mtime;
    entity.localSize = created.first().size;
    entity.localHash = created.first().hash;
    journal.putEntity(entity);

    QFile::rename(path, dir.path() + QStringLiteral("/new.md"));
    const QList<SyncScanner::Change> renamed = SyncScanner::scan(vault, journal, contentKey);
    ASSERT_EQ(renamed.size(), 1);
    EXPECT_EQ(renamed.first().kind, SyncScanner::Change::Renamed);
    EXPECT_EQ(renamed.first().entityID, entity.entityID);
    EXPECT_EQ(renamed.first().oldPath, QStringLiteral("old.md"));
    EXPECT_EQ(renamed.first().path, QStringLiteral("new.md"));
}

// writeBase() goes through QSaveFile: a crash between truncate and write used
// to leave an empty base, and the next cycle made a conflict copy of a
// two-device edit instead of merging it. The stand-in for the crash is a
// write that cannot finish: with the base directory read-only QSaveFile
// cannot create its temporary file, so the old base must survive untouched,
// where a truncating QFile happily emptied the (still writable) file first.
TEST(SyncJournalTest, FailedWriteBaseKeepsThePreviousBase)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores file permissions";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTemporaryDir dataHome;
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    SyncJournal journal(dir.path());
    ASSERT_TRUE(journal.isOpen());

    const QByteArray id = SyncCrypto::randomID();
    journal.writeBase(id, QStringLiteral("one"));
    ASSERT_EQ(journal.readBase(id), QStringLiteral("one"));

    const QString baseDir = SyncJournal::dataDirectoryFor(dir.path()) + QStringLiteral("/base");
    ASSERT_TRUE(QFile::setPermissions(baseDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    journal.writeBase(id, QStringLiteral("two"));
    const QString after = journal.readBase(id);
    QFile::setPermissions(baseDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    EXPECT_EQ(after, QStringLiteral("one"));

    journal.writeBase(id, QStringLiteral("three")); // and it works again once the directory is writable
    EXPECT_EQ(journal.readBase(id), QStringLiteral("three"));
}

// ---------------------------------------------------------------------
// Two devices, a real server: the end-to-end scenarios
// ---------------------------------------------------------------------

TEST_F(SyncIntegrationTest, CreateEditRenameDeleteSyncBetweenTwoDevices)
{
    writeNote(vaultA.path(), QStringLiteral("Welcome.md"), QStringLiteral("first line\nsecond line"));

    SyncEngine engineA;
    Watcher watchA(engineA);
    QSignalSpy asked(&engineA, &SyncEngine::deviceAwaitingApproval);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle()) << watchA.lastMessage.toStdString();
    EXPECT_EQ(watchA.pushed, 1);

    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle()) << watchB.lastMessage.toStdString();
    EXPECT_EQ(watchB.pulled, 1);
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/Welcome.md")), QStringLiteral("first line\nsecond line"));

    // Edit on A, B picks it up.
    writeNote(vaultA.path(), QStringLiteral("Welcome.md"), QStringLiteral("first line EDITED\nsecond line"));
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/Welcome.md")),
             QStringLiteral("first line EDITED\nsecond line"));
    EXPECT_TRUE(asked.isEmpty()) << "a device with the password is never an approval to make";

    // Rename (and move into a folder) on A.
    QDir(vaultA.path()).mkpath(QStringLiteral("folder"));
    QFile::rename(vaultA.path() + QStringLiteral("/Welcome.md"), vaultA.path() + QStringLiteral("/folder/Renamed.md"));
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_FALSE(QFile::exists(vaultB.path() + QStringLiteral("/Welcome.md")));
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/folder/Renamed.md")),
             QStringLiteral("first line EDITED\nsecond line"));

    // Delete on A.
    QFile::remove(vaultA.path() + QStringLiteral("/folder/Renamed.md"));
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_FALSE(QFile::exists(vaultB.path() + QStringLiteral("/folder/Renamed.md")));
}

TEST_F(SyncIntegrationTest, AttachmentSyncsThroughTheBlobStore)
{
    QByteArray bytes(200000, '\0'); // bigger than one secretstream chunk
    for (int i = 0; i < bytes.size(); ++i)
        bytes[i] = char(i % 256);
    writeRaw(vaultA.path() + QStringLiteral("/photo.png"), bytes);

    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());

    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());

    QFile received(vaultB.path() + QStringLiteral("/photo.png"));
    ASSERT_TRUE(received.open(QIODevice::ReadOnly));
    EXPECT_EQ(received.readAll(), bytes);
}

TEST_F(SyncIntegrationTest, OfflineEditsOnDifferentNotesBothSurvive)
{
    writeNote(vaultA.path(), QStringLiteral("one.md"), QStringLiteral("one"));
    writeNote(vaultA.path(), QStringLiteral("two.md"), QStringLiteral("two"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());

    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());

    // Both offline from each other's next edit.
    writeNote(vaultA.path(), QStringLiteral("one.md"), QStringLiteral("one, edited on A"));
    writeNote(vaultB.path(), QStringLiteral("two.md"), QStringLiteral("two, edited on B"));

    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle()); // pushes its own edit, pulls A's
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle()); // pulls B's edit

    EXPECT_EQ(readNoteText(vaultA.path() + QStringLiteral("/one.md")), QStringLiteral("one, edited on A"));
    EXPECT_EQ(readNoteText(vaultA.path() + QStringLiteral("/two.md")), QStringLiteral("two, edited on B"));
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/one.md")), QStringLiteral("one, edited on A"));
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/two.md")), QStringLiteral("two, edited on B"));
}

TEST_F(SyncIntegrationTest, OfflineEditsOnDisjointLinesOfTheSameNoteMergeCleanly)
{
    writeNote(vaultA.path(), QStringLiteral("shared.md"), QStringLiteral("alpha\nbeta\ngamma\ndelta"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());

    writeNote(vaultA.path(), QStringLiteral("shared.md"), QStringLiteral("ALPHA\nbeta\ngamma\ndelta"));
    writeNote(vaultB.path(), QStringLiteral("shared.md"), QStringLiteral("alpha\nbeta\ngamma\nDELTA"));

    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_EQ(watchB.conflicts, 0);
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/shared.md")), QStringLiteral("ALPHA\nbeta\ngamma\nDELTA"));

    // No conflict-copy file should have appeared.
    EXPECT_FALSE(QDir(vaultB.path()).entryList(QStringList() << QStringLiteral("*conflict*")).size());
}

TEST_F(SyncIntegrationTest, AnEditFromAnotherDeviceKeepsThisFilesLineEndingsAndBom)
{
    // Only a note's text travels; how this device's copy ends its lines,
    // and whether it starts with a BOM, is this copy's own.
    writeNote(vaultA.path(), QStringLiteral("crlf.md"), QStringLiteral("one\ntwo"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());
    const QString onB = vaultB.path() + QStringLiteral("/crlf.md");
    writeRaw(onB, QByteArrayLiteral("\xEF\xBB\xBFone\r\ntwo"));
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());

    // Two edits on A, each pulled on its own: the second comes in on top of
    // a copy the first left in agreement with the server.
    for (const QString &text : {QStringLiteral("one\ntwo\nthree"), QStringLiteral("one\ntwo\nthree\nfour")}) {
        engineA.syncNow();
        ASSERT_TRUE(watchA.waitForCycle());
        writeNote(vaultA.path(), QStringLiteral("crlf.md"), text);
        engineA.syncNow();
        ASSERT_TRUE(watchA.waitForCycle());
        engineB.syncNow();
        ASSERT_TRUE(watchB.waitForCycle());
        QByteArray expected = "\xEF\xBB\xBF" + text.toUtf8();
        expected.replace("\n", "\r\n");
        QFile file(onB);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), expected);
    }
    // And a copy left alone is not pushed again on every sync.
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_EQ(watchB.pushed, 0);
}

TEST_F(SyncIntegrationTest, ATrueConflictWritesAConflictCopyAndKeepsLocalInPlace)
{
    writeNote(vaultA.path(), QStringLiteral("shared.md"), QStringLiteral("alpha\nbeta\ngamma"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());

    writeNote(vaultA.path(), QStringLiteral("shared.md"), QStringLiteral("alpha\nBETA-FROM-A\ngamma"));
    writeNote(vaultB.path(), QStringLiteral("shared.md"), QStringLiteral("alpha\nBETA-FROM-B\ngamma"));

    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());

    EXPECT_EQ(watchB.conflicts, 1);
    // Local (B's own edit) is left exactly as it was.
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/shared.md")), QStringLiteral("alpha\nBETA-FROM-B\ngamma"));
    // A conflict copy carrying A's text exists alongside it.
    const QStringList conflictFiles =
        QDir(vaultB.path()).entryList(QStringList() << QStringLiteral("shared (sync conflict*"));
    ASSERT_EQ(conflictFiles.size(), 1);
    EXPECT_EQ(readNoteText(vaultB.path() + QLatin1Char('/') + conflictFiles.first()),
             QStringLiteral("alpha\nBETA-FROM-A\ngamma"));
}

TEST_F(SyncIntegrationTest, AnEditBeatsADeleteInBothDirections)
{
    writeNote(vaultA.path(), QStringLiteral("shared.md"), QStringLiteral("original text"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchB.waitForCycle());

    // B edits (does not sync yet); A deletes and pushes the delete first.
    writeNote(vaultB.path(), QStringLiteral("shared.md"), QStringLiteral("B's edit, offline"));
    QFile::remove(vaultA.path() + QStringLiteral("/shared.md"));
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());

    // B pulls the delete: the edit beats it, the file survives.
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_TRUE(QFile::exists(vaultB.path() + QStringLiteral("/shared.md")));
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/shared.md")), QStringLiteral("B's edit, offline"));

    // B's next scan notices the file has no known entity any more (the
    // delete dropped it from B's journal) and re-pushes it fresh.
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_EQ(watchB.pushed, 1);

    // A pulls that fresh upsert: the note reappears, undeleted.
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    EXPECT_TRUE(QFile::exists(vaultA.path() + QStringLiteral("/shared.md")));
    EXPECT_EQ(readNoteText(vaultA.path() + QStringLiteral("/shared.md")), QStringLiteral("B's edit, offline"));
}

TEST_F(SyncIntegrationTest, NoPlaintextReachesTheServer)
{
    // Deliberately distinctive strings that must never appear server-side,
    // in any form: not the body, not the file name, not a tag.
    const QString secretBody = QStringLiteral("The launch codes are TROMBONE-RUTABAGA-4471.");
    writeNote(vaultA.path(), QStringLiteral("Presidential Secrets.md"),
             QStringLiteral("---\ntags: [zk9x-top-secret]\n---\n\n") + secretBody);
    writeRaw(vaultA.path() + QStringLiteral("/attachment-with-a-revealing-name.bin"),
            QByteArrayLiteral("ANOTHER-MARKER-STRING-789"));

    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    ASSERT_GE(watchA.pushed, 1);

    // The account's email is legitimate server-side metadata (docs/SYNC.md:
    // the accounts table has to have *something* to look accounts up by) -
    // not a content leak, so it is deliberately not a needle here. The
    // password itself must never appear anywhere: only loginKey (derived,
    // one-way, from masterKey - itself derived from the password) ever
    // reaches the server.
    const QList<QByteArray> needles = {
        secretBody.toUtf8(),
        QByteArrayLiteral("Presidential Secrets"),
        QByteArrayLiteral("zk9x-top-secret"),
        QByteArrayLiteral("attachment-with-a-revealing-name"),
        QByteArrayLiteral("ANOTHER-MARKER-STRING-789"),
        password.toUtf8(),
    };
    for (const auto &[path, bytes] : allFilesUnder(serverData.path())) {
        for (const QByteArray &needle : needles) {
            EXPECT_FALSE(bytes.contains(needle))
                << "found " << needle.toStdString() << " in " << path.toStdString();
        }
    }
}

TEST_F(SyncIntegrationTest, PasswordChangeKeepsEverythingReadable)
{
    writeNote(vaultA.path(), QStringLiteral("note.md"), QStringLiteral("content before the change"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());

    const QString newPassword = QStringLiteral("a different, also-decent passphrase");
    engineA.changePassword(newPassword);
    watchA.pump(500); // changePassword() has no signal of its own; give the worker thread a moment

    // A new note after the change, still syncable on the same session.
    writeNote(vaultA.path(), QStringLiteral("note2.md"), QStringLiteral("content after the change"));
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());

    // A fresh engine, signing in with the *new* password, reads everything.
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(newPassword));
    ASSERT_TRUE(watchB.waitForCycle());
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/note.md")), QStringLiteral("content before the change"));
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/note2.md")), QStringLiteral("content after the change"));
}

TEST_F(SyncIntegrationTest, ASecondDeviceEnrolledByLockboxReadsEverything)
{
    writeNote(vaultA.path(), QStringLiteral("note.md"), QStringLiteral("only ever synced via lockbox"));
    SyncEngine engineA;
    Watcher watchA(engineA);
    engineA.start(vaultA.path(), serverURL, email, fixedPassword(password));
    ASSERT_TRUE(watchA.waitForCycle());
    ASSERT_GE(watchA.pushed, 1);

    // Device B never derives wrapKey's itemsKeys fetch path - see
    // docs/SYNC.md "Device enrolment with lockboxes", path 2.
    SyncEngine engineB;
    Watcher watchB(engineB);
    engineB.start(vaultB.path(), serverURL, email, fixedPassword(password), /*viaLockbox=*/true);
    ASSERT_TRUE(watchB.waitForStatus(SyncEngine::Status::WaitingForApproval)) << watchB.lastMessage.toStdString();
    EXPECT_EQ(watchB.finishedCount, 0) << "must not have synced anything without a lockbox";

    // A's next cycle notices the pending device and asks; nothing is sealed
    // until a person approves it, comparing the code B shows.
    QSignalSpy asked(&engineA, &SyncEngine::deviceAwaitingApproval);
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());
    ASSERT_EQ(asked.size(), 1);
    const QString code = asked.at(0).at(1).toString();
    EXPECT_TRUE(watchB.lastMessage.contains(code)) << "B shows " << watchB.lastMessage.toStdString();
    QSignalSpy statusB(&engineB, &SyncEngine::statusChanged);
    engineB.syncNow();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return std::any_of(statusB.cbegin(), statusB.cend(), [](const QList<QVariant> &args) {
            return args.at(0).value<SyncEngine::Status>() == SyncEngine::Status::WaitingForApproval;
        });
    }, 20000)) << "B's own cycle must still be waiting";
    EXPECT_EQ(watchB.finishedCount, 0) << "not approved: still no keys";

    engineA.approveDevice(asked.at(0).at(0).toByteArray());
    engineA.syncNow();
    ASSERT_TRUE(watchA.waitForCycle());

    // B tries again: this time the lockbox is there.
    engineB.syncNow();
    ASSERT_TRUE(watchB.waitForCycle()) << watchB.lastMessage.toStdString();
    EXPECT_EQ(readNoteText(vaultB.path() + QStringLiteral("/note.md")), QStringLiteral("only ever synced via lockbox"));
}

// ---------------------------------------------------------------------
// Keys kept in the keyring: the password is asked once per device
// (docs/SYNC.md "Keys kept in the keyring")
// ---------------------------------------------------------------------

TEST_F(SyncIntegrationTest, AfterTheFirstUnlockANewEngineStartsWithoutAskingForThePassword)
{
    writeNote(vaultA.path(), QStringLiteral("Welcome.md"), QStringLiteral("hello"));
    auto asked = std::make_shared<std::atomic<int>>(0);
    {
        SyncEngine engine;
        Watcher watch(engine);
        engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
        ASSERT_TRUE(watch.waitForCycle()) << watch.lastMessage.toStdString();
        EXPECT_EQ(asked->load(), 1);
    }

    // One entry, scoped to this server and this device, and what it holds is
    // three 32-byte keys behind a version byte - not the password.
    const QStringList entries = keyEntries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries.first().startsWith(QStringLiteral("keys/") + serverURL + QLatin1Char('/')));
    {
        const QMutexLocker lock(&g_secretsMutex);
        const QByteArray blob = g_secrets.value(entries.first());
        EXPECT_EQ(blob.size(), 1 + 3 * 32);
        EXPECT_FALSE(blob.contains(password.toUtf8()));
    }

    // The next session, same vault: unlocked from the keyring, the prompt is never called.
    writeNote(vaultA.path(), QStringLiteral("Second.md"), QStringLiteral("more"));
    SyncEngine again;
    Watcher watchAgain(again);
    again.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
    ASSERT_TRUE(watchAgain.waitForCycle()) << watchAgain.lastMessage.toStdString();
    EXPECT_EQ(asked->load(), 1) << "the keyring had the keys; nobody should have been asked";
    EXPECT_EQ(watchAgain.pushed, 1); // and it really synced, content key included
}

TEST_F(SyncIntegrationTest, AStoredKeyTheServerRefusesIsDroppedAndThePasswordAsked)
{
    auto asked = std::make_shared<std::atomic<int>>(0);
    {
        SyncEngine engine;
        Watcher watch(engine);
        engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
        ASSERT_TRUE(watch.waitForCycle());
    }
    const QStringList entries = keyEntries();
    ASSERT_EQ(entries.size(), 1);
    QByteArray stale(1 + 3 * 32, '\0'); // well-formed, but not this account's keys
    stale[0] = 1;
    for (int i = 1; i < stale.size(); ++i)
        stale[i] = char(i);
    {
        const QMutexLocker lock(&g_secretsMutex);
        g_secrets.insert(entries.first(), stale);
    }

    SyncEngine engine;
    Watcher watch(engine);
    engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
    ASSERT_TRUE(watch.waitForCycle()) << watch.lastMessage.toStdString();
    EXPECT_EQ(asked->load(), 2);
    const QMutexLocker lock(&g_secretsMutex);
    EXPECT_NE(g_secrets.value(entries.first()), stale) << "the good keys replaced the stale entry";
    EXPECT_EQ(g_secrets.value(entries.first()).size(), 1 + 3 * 32);
}

TEST_F(SyncIntegrationTest, AMalformedStoredEntryIsIgnoredAndThePasswordAsked)
{
    auto asked = std::make_shared<std::atomic<int>>(0);
    {
        SyncEngine engine;
        Watcher watch(engine);
        engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
        ASSERT_TRUE(watch.waitForCycle());
    }
    const QStringList entries = keyEntries();
    ASSERT_EQ(entries.size(), 1);
    {
        const QMutexLocker lock(&g_secretsMutex);
        g_secrets.insert(entries.first(), QByteArrayLiteral("garbage"));
    }
    SyncEngine engine;
    Watcher watch(engine);
    engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
    ASSERT_TRUE(watch.waitForCycle()) << watch.lastMessage.toStdString();
    EXPECT_EQ(asked->load(), 2);
}

TEST_F(SyncIntegrationTest, APasswordChangedOnAnotherDeviceMakesTheStoredKeysStale)
{
    const QString newPassword = QStringLiteral("a different, also-decent passphrase");
    auto asked = std::make_shared<std::atomic<int>>(0);
    {
        SyncEngine engine;
        Watcher watch(engine);
        engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
        ASSERT_TRUE(watch.waitForCycle());
    }
    {
        SyncEngine other; // the second device changes the password
        Watcher watchOther(other);
        other.start(vaultB.path(), serverURL, email, fixedPassword(password));
        ASSERT_TRUE(watchOther.waitForCycle());
        other.changePassword(newPassword);
        watchOther.pump(500);
    }
    const QStringList before = keyEntries();
    ASSERT_EQ(before.size(), 2); // one entry per device
    QHash<QString, QByteArray> snapshot;
    {
        const QMutexLocker lock(&g_secretsMutex);
        for (const QString &name : before)
            snapshot.insert(name, g_secrets.value(name));
    }

    // Device A still holds the old password's keys: refused, dropped, asked.
    SyncEngine engine;
    Watcher watch(engine);
    engine.start(vaultA.path(), serverURL, email, countingPrompt(newPassword, asked));
    ASSERT_TRUE(watch.waitForCycle()) << watch.lastMessage.toStdString();
    EXPECT_EQ(asked->load(), 2);
    int changed = 0;
    const QMutexLocker lock(&g_secretsMutex);
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it)
        changed += g_secrets.value(it.key()) != it.value();
    EXPECT_EQ(changed, 1) << "device A's entry now holds the new keys; B's already did";
}

TEST_F(SyncIntegrationTest, AnUnreachableServerKeepsTheStoredKeys)
{
    auto asked = std::make_shared<std::atomic<int>>(0);
    {
        SyncEngine engine;
        Watcher watch(engine);
        engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
        ASSERT_TRUE(watch.waitForCycle());
    }
    server.reset(); // offline

    SyncEngine engine;
    Watcher watch(engine);
    engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
    ASSERT_TRUE(watch.waitForStatus(SyncEngine::Status::Error, 20000));
    EXPECT_EQ(asked->load(), 1) << "offline is not a reason to ask again";
    EXPECT_EQ(keyEntries().size(), 1) << "...nor to forget the keys";
}

TEST_F(SyncIntegrationTest, ForgettingTheKeysRemovesTheEntryAndTheNextStartAsks)
{
    auto asked = std::make_shared<std::atomic<int>>(0);
    SyncEngine engine;
    Watcher watch(engine);
    engine.start(vaultA.path(), serverURL, email, countingPrompt(password, asked));
    ASSERT_TRUE(watch.waitForCycle());
    ASSERT_EQ(keyEntries().size(), 1);

    engine.forgetStoredKeys();
    ASSERT_TRUE(watch.waitForStatus(SyncEngine::Status::NeedsPassword, 10000));
    EXPECT_TRUE(keyEntries().isEmpty());

    // With nothing in the keyring and nobody to ask, a start waits for a
    // sign-in rather than failing.
    SyncEngine next;
    Watcher watchNext(next);
    auto nobody = std::make_shared<std::atomic<int>>(0);
    next.start(vaultA.path(), serverURL, email, countingPrompt(QString(), nobody));
    ASSERT_TRUE(watchNext.waitForStatus(SyncEngine::Status::NeedsPassword, 10000));
    EXPECT_GE(nobody->load(), 1);
    EXPECT_TRUE(keyEntries().isEmpty());
}

TEST_F(SyncIntegrationTest, ANoChangeSyncOnA20000NoteVaultIsFast)
{
    // The task's own performance bar, adapted: a vault of 20 000 notes
    // must do a no-change sync quickly. Generating 20 000 real files
    // (tools/gen_bench_vault.py's job for the app itself) is overkill for
    // this suite's budget, so this measures the scanner's own no-change
    // pass - the part of a sync cycle whose cost scales with vault size -
    // over 20 000 synthetic notes directly, which is what the mtime/size
    // cheap-skip in SyncScanner::scan is actually there to keep cheap.
    QTemporaryDir bigVault;
    ASSERT_TRUE(bigVault.isValid());
    QTemporaryDir bigDataHome;
    qputenv("XDG_DATA_HOME", bigDataHome.path().toUtf8());
    const Vault vault(bigVault.path());
    SyncJournal journal(bigVault.path());
    ASSERT_TRUE(journal.isOpen());
    const QByteArray contentKey = SyncCrypto::randomKey();

    constexpr int kNotes = 20000;
    for (int i = 0; i < kNotes; ++i) {
        const QString path = writeNote(bigVault.path(), QStringLiteral("note-%1.md").arg(i, 5, 10, QLatin1Char('0')),
                                       QStringLiteral("content of note %1").arg(i));
        const QFileInfo info(path);
        SyncJournal::Entity entity;
        entity.entityID = SyncCrypto::randomID();
        entity.path = QStringLiteral("note-%1.md").arg(i, 5, 10, QLatin1Char('0'));
        entity.entityType = QStringLiteral("note");
        entity.localMtime = info.lastModified().toMSecsSinceEpoch();
        entity.localSize = info.size();
        entity.localHash = SyncCrypto::contentAddress(contentKey, readNoteText(path).toUtf8());
        journal.putEntity(entity);
    }

    QElapsedTimer clock;
    clock.start();
    const QList<SyncScanner::Change> changes = SyncScanner::scan(vault, journal, contentKey);
    const qint64 ms = clock.elapsed();
    std::cout << "[ INFO     ] no-change scan of " << kNotes << " notes: " << ms << " ms\n";
    EXPECT_TRUE(changes.isEmpty());
    EXPECT_LT(ms, 5000);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Never the real OS keyring: a run must not leave device keys in it,
    // and an unanswered secret-service call cost each engine 15 s.
    SyncEngine::setSecretStore({[](const QString &key, QByteArray *value) {
                                    const QMutexLocker lock(&g_secretsMutex);
                                    if (!g_secrets.contains(key))
                                        return false;
                                    *value = g_secrets.value(key);
                                    return true;
                                },
                                [](const QString &key, const QByteArray &value) {
                                    const QMutexLocker lock(&g_secretsMutex);
                                    g_secrets.insert(key, value);
                                    return true;
                                },
                                [](const QString &key) {
                                    const QMutexLocker lock(&g_secretsMutex);
                                    return g_secrets.remove(key) > 0;
                                }});
    // Deliberately *not* QStandardPaths::setTestModeEnabled(true), unlike
    // test_index.cpp's own main(): test mode redirects
    // QStandardPaths::writableLocation() to a fixed ~/.qttest/... - which
    // Qt itself creates directly under the real home directory, never
    // under /tmp, and never cleans up - overriding the per-test
    // XDG_DATA_HOME each fixture sets below to isolate SyncJournal's data
    // directory. Every test then shared and kept appending to the same
    // real, permanent, ever-growing ~/.qttest/share/noteAppC/sync/, which
    // is both a stray-file-outside-/tmp problem and - once it had
    // accumulated a couple hundred stale vault-digest directories over a
    // long debugging session - a real performance cliff: directory
    // operations on it started taking upwards of ten seconds, which
    // showed up as inexplicable multi-second stalls with no corresponding
    // line of code, deep inside otherwise fully-instrumented sync cycles.
    // Leaving XDG_DATA_HOME (and XDG_CACHE_HOME/XDG_CONFIG_HOME, unused by
    // this suite but harmless to set) as the per-fixture QTemporaryDir
    // actually does is what SyncJournal::dataDirectoryFor() is written to
    // respect - see SyncIntegrationTest::SetUp() and the two
    // fixture-less tests that set it by hand.
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
