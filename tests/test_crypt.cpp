// SPDX-License-Identifier: GPL-3.0-or-later
// Encrypted notes (libs/ext/crypt): the file format and the session that
// holds the keys. Argon2id runs at libsodium's minimum here, so the suite
// stays fast; the format stores whatever params a file was sealed with.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "EncryptionSession.h"
#include "NoteCrypt.h"

#include <sodium.h>

using namespace noteAppC::crypt;

namespace {

const Params kFast{crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN / 1024};

} // namespace

TEST(NoteCryptTest, RoundTripsAndHidesThePlaintext)
{
    ASSERT_TRUE(available());
    const Key key = derive(QStringLiteral("correct horse"), newSalt(), kFast);
    ASSERT_TRUE(key.isValid());
    const QByteArray text = QByteArrayLiteral("# Secret\n\nThe plan is ready.\n");
    const QByteArray file = seal(key, text);
    EXPECT_FALSE(file.contains("plan")) << "plaintext on disk";
    EXPECT_TRUE(file.startsWith("NAPCLOCK"));
    ASSERT_TRUE(open(key, file).has_value());
    EXPECT_EQ(*open(key, file), text);
    // A fresh nonce each time: the same text never seals to the same bytes.
    EXPECT_NE(seal(key, text), file);
    EXPECT_EQ(*open(key, seal(key, QByteArray())), QByteArray());
}

TEST(NoteCryptTest, AWrongPasswordOrAnyChangedByteFails)
{
    const QByteArray salt = newSalt();
    const Key key = derive(QStringLiteral("correct horse"), salt, kFast);
    const Key wrong = derive(QStringLiteral("correct horsE"), salt, kFast);
    const QByteArray file = seal(key, QByteArrayLiteral("secret"));
    EXPECT_FALSE(open(wrong, file).has_value());
    for (int i = 0; i < file.size(); ++i) {
        QByteArray tampered = file;
        tampered[i] = char(tampered.at(i) ^ 0x01);
        EXPECT_FALSE(open(key, tampered).has_value()) << "byte " << i << " is not authenticated";
    }
    EXPECT_FALSE(open(key, file.left(file.size() - 1)).has_value());
    EXPECT_FALSE(header(QByteArrayLiteral("# just markdown\n")).has_value());
}

TEST(NoteCryptTest, TheHeaderCarriesSaltAndParams)
{
    const Key key = derive(QStringLiteral("pw"), newSalt(), kFast);
    const auto head = header(seal(key, QByteArrayLiteral("x")));
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->salt, key.salt());
    EXPECT_EQ(head->params.opslimit, kFast.opslimit);
    EXPECT_EQ(head->params.memlimitKiB, kFast.memlimitKiB);
    EXPECT_TRUE(isEncryptedNote(QStringLiteral("/v/Secret.mdlock")));
    EXPECT_FALSE(isEncryptedNote(QStringLiteral("/v/Secret.md")));
}

TEST(EncryptionSessionTest, AsksOnceThenRemembersUntilLocked)
{
    EncryptionSession session;
    session.setParams(kFast);
    int asked = 0;
    QString answer = QStringLiteral("hunter2 hunter2");
    session.setPrompt([&](bool, bool) {
        ++asked;
        return answer;
    });
    // No encrypted note yet: the first seal sets the password.
    const QByteArray first = session.seal(QByteArrayLiteral("one"), QByteArray());
    ASSERT_FALSE(first.isEmpty());
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(*session.open(first), QByteArrayLiteral("one"));
    const QByteArray second = session.seal(QByteArrayLiteral("two"), first);
    EXPECT_EQ(asked, 1) << "unlocked: no second prompt";

    QSignalSpy locked(&session, &EncryptionSession::locked);
    session.lock();
    EXPECT_EQ(locked.size(), 1);
    EXPECT_FALSE(session.isUnlocked());

    // Locked: the next open asks again, and a wrong answer is asked again.
    int tries = 0;
    session.setPrompt([&](bool creating, bool retry) {
        EXPECT_FALSE(creating);
        EXPECT_EQ(retry, tries > 0);
        return ++tries < 3 ? QStringLiteral("wrong") : answer;
    });
    EXPECT_EQ(*session.open(second), QByteArrayLiteral("two"));
    EXPECT_EQ(tries, 3);
}

TEST(EncryptionSessionTest, CancelOrThreeWrongPasswordsStayLocked)
{
    EncryptionSession session;
    session.setParams(kFast);
    session.setPrompt([](bool, bool) { return QStringLiteral("right"); });
    const QByteArray file = session.seal(QByteArrayLiteral("x"), QByteArray());
    session.lock();

    session.setPrompt([](bool, bool) { return QString(); });
    EXPECT_FALSE(session.open(file).has_value());
    int tries = 0;
    session.setPrompt([&](bool, bool) {
        ++tries;
        return QStringLiteral("wrong");
    });
    QString error;
    EXPECT_FALSE(session.open(file, &error).has_value());
    EXPECT_EQ(tries, 3);
    EXPECT_FALSE(session.isUnlocked());
}

TEST(EncryptionSessionTest, LocksItselfWhenIdle)
{
    EncryptionSession session;
    session.setParams(kFast);
    session.setPrompt([](bool, bool) { return QStringLiteral("pw"); });
    ASSERT_FALSE(session.seal(QByteArrayLiteral("x"), QByteArray()).isEmpty());
    QSignalSpy locked(&session, &EncryptionSession::locked);
    session.setAutoLockMs(300);
    for (int i = 0; i < 4; ++i) { // activity keeps it open past the interval
        QTest::qWait(150);
        session.noteActivity();
    }
    EXPECT_TRUE(locked.isEmpty());
    EXPECT_TRUE(session.isUnlocked());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !locked.isEmpty(); }, 2000));
    EXPECT_FALSE(session.isUnlocked());
}

TEST(EncryptionSessionTest, RekeyedFilesOpenAndOldOnesStillDo)
{
    EncryptionSession session;
    session.setParams(kFast);
    session.setPrompt([](bool, bool) { return QStringLiteral("old password"); });
    const QByteArray old = session.seal(QByteArrayLiteral("old"), QByteArray());
    ASSERT_TRUE(session.rekey(QStringLiteral("new password")));
    const QByteArray fresh = session.seal(QByteArrayLiteral("new"), QByteArray());
    EXPECT_NE(header(old)->salt, header(fresh)->salt);
    EXPECT_EQ(*session.open(old), QByteArrayLiteral("old")); // re-sealing each file is the caller's
    session.lock();
    session.setPrompt([](bool, bool) { return QStringLiteral("new password"); });
    EXPECT_EQ(*session.open(fresh), QByteArrayLiteral("new"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
