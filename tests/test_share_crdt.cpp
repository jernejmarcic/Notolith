// CrdtDoc (libs/ext/share): one suite, run over every backend. A backend the
// build lacks skips; see docs/SHARE.md.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <algorithm>
#include <cstdio>
#include <vector>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "CrdtDoc.h"
#include "Perf.h"

using namespace noteAppC;
using Backend = CrdtDoc::Backend;

namespace {

/// `edits` applied one after the other, each to what the one before left -
/// the contract MarkdownEditor::applyExternalEdit is fed under.
QString applyAll(QString text, const QList<CrdtEdit> &edits)
{
    for (const CrdtEdit &edit : edits) {
        EXPECT_LE(edit.at + edit.removed, text.size()) << "edit outside the text";
        text.replace(edit.at, edit.removed, edit.inserted);
    }
    return text;
}

class CrdtDocTest : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() override
    {
        if (!CrdtDoc::available(GetParam()))
            GTEST_SKIP() << "backend not in this build";
    }

    std::unique_ptr<CrdtDoc> make(quint64 clientID) { return CrdtDoc::create(GetParam(), clientID); }

    /// Deliver `update` to `doc`, insisting that it decodes.
    static QList<CrdtEdit> deliver(CrdtDoc &doc, const QByteArray &update)
    {
        bool ok = false;
        const QList<CrdtEdit> edits = doc.applyUpdate(update, &ok);
        EXPECT_TRUE(ok);
        return edits;
    }

    /// Bring `to` up to date with `from` the way two peers meeting do.
    static void catchUp(CrdtDoc &to, const CrdtDoc &from) { deliver(to, from.diffSince(to.stateVector())); }
};

} // namespace

TEST_P(CrdtDocTest, TwoDocsConvergeOnConcurrentInserts)
{
    auto a = make(1), b = make(2);
    deliver(*b, a->localEdit(0, 0, QStringLiteral("hello")));
    ASSERT_EQ(b->text(), QStringLiteral("hello"));

    // Both type while the other's update is still in flight, twice at the
    // very same spot.
    const QByteArray fromA = a->localEdit(5, 0, QStringLiteral(" world"));
    const QByteArray fromB = b->localEdit(0, 0, QStringLiteral(">> "));
    const QByteArray sameSpotA = a->localEdit(0, 0, QStringLiteral("A"));
    const QByteArray sameSpotB = b->localEdit(0, 0, QStringLiteral("B"));
    deliver(*a, fromB);
    deliver(*a, sameSpotB);
    deliver(*b, fromA);
    deliver(*b, sameSpotA);

    EXPECT_EQ(a->text(), b->text());
    for (const QString &part : {QStringLiteral("hello"), QStringLiteral(" world"), QStringLiteral(">> "),
                                QStringLiteral("A"), QStringLiteral("B")})
        EXPECT_TRUE(a->text().contains(part)) << qPrintable(part) << " lost from " << qPrintable(a->text());
    EXPECT_EQ(a->text().size(), 5 + 6 + 3 + 1 + 1);
}

TEST_P(CrdtDocTest, ConcurrentDeleteAndInsertConverge)
{
    auto a = make(1), b = make(2);
    deliver(*b, a->localEdit(0, 0, QStringLiteral("abcdef")));

    const QByteArray deletion = a->localEdit(2, 2, QString());                 // abef
    const QByteArray insertion = b->localEdit(3, 0, QStringLiteral("XY"));    // abcXYdef
    deliver(*a, insertion);
    deliver(*b, deletion);

    EXPECT_EQ(a->text(), b->text());
    EXPECT_EQ(a->text(), QStringLiteral("abXYef")); // the insert survives inside what the other deleted
}

TEST_P(CrdtDocTest, OffsetsAreUtf16SoAnEmojiRoundTrips)
{
    auto a = make(1), b = make(2);
    const QString grin = QStringLiteral("a\U0001F600b"); // one code point, two UTF-16 units
    ASSERT_EQ(grin.size(), 4);
    const QByteArray first = a->localEdit(0, 0, grin);
    EXPECT_EQ(a->text(), grin);
    deliver(*b, first);
    EXPECT_EQ(b->text(), grin);
    EXPECT_EQ(b->text().size(), 4);

    // An offset after the emoji is 3 in QString terms; it is 5 in UTF-8.
    const QList<CrdtEdit> inserted = deliver(*b, a->localEdit(3, 0, QStringLiteral("!")));
    ASSERT_EQ(inserted.size(), 1);
    EXPECT_EQ(inserted[0].at, 3);
    EXPECT_EQ(b->text(), QStringLiteral("a\U0001F600!b"));

    // Removing the emoji is a length-2 range, and arrives as one.
    const QList<CrdtEdit> removed = deliver(*a, b->localEdit(1, 2, QString()));
    ASSERT_EQ(removed.size(), 1);
    EXPECT_EQ(removed[0].at, 1);
    EXPECT_EQ(removed[0].removed, 2);
    EXPECT_EQ(a->text(), QStringLiteral("a!b"));
    EXPECT_EQ(b->text(), a->text());

    // Replacing one emoji while the other side appends after the lot.
    a->localEdit(0, a->text().size(), QStringLiteral("x\U0001F680\U0001F680y"));
    auto c = make(3);
    catchUp(*c, *a);
    const QByteArray replaced = c->localEdit(1, 2, QStringLiteral("R"));
    deliver(*c, a->localEdit(6, 0, QStringLiteral("end")));
    deliver(*a, replaced);
    EXPECT_EQ(a->text(), QStringLiteral("xR\U0001F680yend"));
    EXPECT_EQ(c->text(), a->text());
}

TEST_P(CrdtDocTest, StateVectorDiffCatchesUpAnOfflinePeer)
{
    auto a = make(1), b = make(2);
    deliver(*b, a->localEdit(0, 0, QStringLiteral("shared start\n")));

    // Offline: neither update is delivered.
    a->localEdit(a->text().size(), 0, QStringLiteral("a typed this\n"));
    a->localEdit(0, 6, QString());
    a->localEdit(0, 0, QStringLiteral("A: "));
    b->localEdit(b->text().size(), 0, QStringLiteral("b typed that\n"));
    ASSERT_NE(a->text(), b->text());

    // Reconnect: swap vectors, send what the other lacks.
    const QByteArray svA = a->stateVector(), svB = b->stateVector();
    const QByteArray forB = a->diffSince(svB), forA = b->diffSince(svA);
    ASSERT_FALSE(forB.isEmpty());
    ASSERT_FALSE(forA.isEmpty());
    deliver(*b, forB);
    deliver(*a, forA);
    EXPECT_EQ(a->text(), b->text());
    EXPECT_TRUE(a->text().contains(QStringLiteral("b typed that")));
    EXPECT_TRUE(a->text().contains(QStringLiteral("a typed this")));

    // A peer that lacks only a little is sent only that little, not the note.
    deliver(*a, b->localEdit(0, 0, QString(2000, QLatin1Char('.'))));
    catchUp(*b, *a);
    const QByteArray tail = a->localEdit(a->text().size(), 0, QStringLiteral("tail"));
    const QByteArray diff = a->diffSince(b->stateVector());
    EXPECT_LT(diff.size(), 100);
    EXPECT_GT(a->encodeState().size(), 2000);
    deliver(*b, diff);
    EXPECT_EQ(a->text(), b->text());
    EXPECT_FALSE(tail.isEmpty());

    // Up to date: a second diff changes nothing, and the vectors agree.
    EXPECT_TRUE(deliver(*b, a->diffSince(b->stateVector())).isEmpty());
    EXPECT_EQ(a->stateVector(), b->stateVector());
    // A vector that is not one is refused, not mistaken for "send everything".
    EXPECT_TRUE(a->diffSince(QByteArrayLiteral("\xff\xff\xff\xff\xff\xff")).isEmpty());
}

TEST_P(CrdtDocTest, DeltaHasTheEditedSignalShape)
{
    auto a = make(1), b = make(2);
    QRandomGenerator rng(20261019);
    const QStringList pieces = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral(" "),
                                QStringLiteral("\n"), QStringLiteral("\U0001F600")};
    auto randomText = [&](int pieceCount) {
        QString s;
        for (int i = 0; i < pieceCount; ++i)
            s += pieces[rng.bounded(int(pieces.size()))];
        return s;
    };

    // What an editor showing b would hold, fed only by the edits.
    QString mirror = applyAll(QString(), deliver(*b, a->localEdit(0, 0, randomText(40))));
    ASSERT_EQ(mirror, b->text());
    ASSERT_FALSE(mirror.isEmpty());

    // One update per edit, and then a burst the peer takes in one go: several
    // separated changes in a single delta, so the retains between them count.
    for (int round = 0; round < 40; ++round) {
        const qsizetype size = a->text().size();
        qsizetype at = rng.bounded(int(size + 1));
        // Never split an emoji: only edit on a boundary.
        while (at > 0 && at < size && a->text()[at].isLowSurrogate())
            ++at;
        qsizetype removed = size == at ? 0 : rng.bounded(int(std::min<qsizetype>(size - at, 6)) + 1);
        while (at + removed < size && a->text()[at + removed].isLowSurrogate())
            ++removed;
        const QByteArray update = a->localEdit(at, removed, rng.bounded(3) ? randomText(rng.bounded(5)) : QString());
        if (update.isEmpty()) // nothing was removed or inserted
            continue;
        mirror = applyAll(mirror, deliver(*b, update));
        ASSERT_EQ(mirror, b->text()) << "round " << round;
        ASSERT_EQ(mirror, a->text());
    }

    auto c = make(3);
    deliver(*c, a->diffSince({})); // c has seen everything so far
    mirror = c->text();
    for (int i = 0; i < 6; ++i) { // a, alone, makes six scattered edits
        const qsizetype size = a->text().size();
        qsizetype at = size * i / 6;
        while (at < size && a->text()[at].isLowSurrogate())
            ++at;
        a->localEdit(at, 0, QStringLiteral("<%1>").arg(i));
        const QChar next = a->text()[at + 3]; // the character the tag was put in front of
        if (i % 2 && !next.isSurrogate())
            a->localEdit(at + 3, 1, QString());
    }
    const QList<CrdtEdit> burst = deliver(*c, a->diffSince(c->stateVector()));
    EXPECT_GT(burst.size(), 3);
    EXPECT_EQ(applyAll(mirror, burst), a->text());
    EXPECT_EQ(c->text(), a->text());
}

TEST_P(CrdtDocTest, EncodedStateReloadsByteIdentical)
{
    auto a = make(1), peer = make(2);
    deliver(*peer, a->localEdit(0, 0, QStringLiteral("# Title\n\nBody \U0001F600 text\n")));
    deliver(*a, peer->localEdit(2, 5, QStringLiteral("Heading")));
    a->localEdit(0, 1, QString());
    deliver(*peer, a->diffSince(peer->stateVector()));

    const QByteArray state = a->encodeState();
    ASSERT_FALSE(state.isEmpty());
    auto reloaded = make(9);
    ASSERT_TRUE(reloaded->loadState(state));
    EXPECT_EQ(reloaded->text(), a->text());
    EXPECT_EQ(reloaded->encodeState(), state);
    EXPECT_EQ(reloaded->stateVector(), a->stateVector());

    // It is a live document, not a snapshot: edits flow both ways.
    deliver(*a, reloaded->localEdit(0, 0, QStringLiteral("from reload ")));
    deliver(*reloaded, a->localEdit(a->text().size(), 0, QStringLiteral("from original")));
    EXPECT_EQ(a->text(), reloaded->text());
    EXPECT_TRUE(a->text().startsWith(QStringLiteral("from reload ")));
    EXPECT_TRUE(a->text().endsWith(QStringLiteral("from original")));
    deliver(*peer, reloaded->diffSince(peer->stateVector())); // and the reload is mergeable with older replicas
    EXPECT_EQ(peer->text(), a->text());

    auto broken = make(10);
    EXPECT_FALSE(broken->loadState(state.left(state.size() / 2)));
    EXPECT_EQ(broken->text(), QString());
}

TEST_P(CrdtDocTest, AMalformedUpdateIsRejectedNotCrashing)
{
    auto doc = make(1), source = make(2);
    deliver(*doc, source->localEdit(0, 0, QStringLiteral("some text \U0001F600 here")));
    const QString text = doc->text();
    const QByteArray vector = doc->stateVector();
    auto untouched = [&] { return doc->text() == text && doc->stateVector() == vector; };

    bool ok = true;
    EXPECT_TRUE(doc->applyUpdate(QByteArray(), &ok).isEmpty());
    EXPECT_FALSE(ok);
    EXPECT_TRUE(untouched());

    // A real update cut short at every length is refused every time.
    source->localEdit(3, 4, QStringLiteral("replacement"));
    const QByteArray real = source->diffSince(vector);
    ASSERT_GT(real.size(), 8);
    for (qsizetype keep = 0; keep < real.size(); ++keep) {
        ok = true;
        doc->applyUpdate(real.left(keep), &ok);
        EXPECT_FALSE(ok) << "prefix of " << keep << " bytes was accepted";
        EXPECT_TRUE(untouched()) << "prefix of " << keep;
    }

    // Noise, and a real update with bytes flipped: whatever the verdict, no
    // crash, and a refusal leaves the document as it was.
    // Refusing must also be cheap: noise once made yrs reserve gigabytes
    // for a client count it had not checked against the bytes it held.
    QRandomGenerator rng(42);
    [[maybe_unused]] QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 4000; ++i) {
        QByteArray noise;
        if (i % 2) {
            noise = real;
            noise[rng.bounded(int(noise.size()))] = char(rng.bounded(256));
        } else {
            for (int n = rng.bounded(64); n > 0; --n)
                noise.append(char(rng.bounded(256)));
        }
        auto victim = make(100 + i % 7);
        victim->localEdit(0, 0, QStringLiteral("x"));
        const QString before = victim->text();
        const QByteArray vectorBefore = victim->stateVector();
        ok = true;
        victim->applyUpdate(noise, &ok);
        if (!ok) {
            EXPECT_EQ(victim->text(), before);
            EXPECT_EQ(victim->stateVector(), vectorBefore);
        }
    }

#ifndef NOTEAPPC_SANITIZED_BUILD
    EXPECT_LT(clock.elapsed(), 4000) << "rejecting 4000 bad updates was slow: a size claim is being believed";
#endif

    // The same doc still works, so nothing was left half done.
    deliver(*doc, real);
    EXPECT_EQ(doc->text(), source->text());
}

TEST_P(CrdtDocTest, ANulCharacterIsJustText)
{
    auto a = make(1), b = make(2);
    const QString withNul = QStringLiteral("a") + QChar(0) + QStringLiteral("b");
    deliver(*b, a->localEdit(0, 0, withNul));
    EXPECT_EQ(a->text(), withNul);
    EXPECT_EQ(b->text(), withNul);
}

TEST_P(CrdtDocTest, AnEditOutsideTheTextIsRefused)
{
    auto doc = make(1);
    doc->localEdit(0, 0, QStringLiteral("abc"));
    EXPECT_TRUE(doc->localEdit(4, 0, QStringLiteral("x")).isEmpty());
    EXPECT_TRUE(doc->localEdit(2, 2, QString()).isEmpty());
    EXPECT_TRUE(doc->localEdit(-1, 0, QStringLiteral("x")).isEmpty());
    EXPECT_EQ(doc->text(), QStringLiteral("abc"));
}

TEST_P(CrdtDocTest, AHugeStateVectorClaimIsRefusedCheaply)
{
    auto doc = make(1);
    doc->localEdit(0, 0, QStringLiteral("abc"));
    const QByteArray vector = doc->stateVector();

    // Five bytes that claim 2^28 entries: yrs would reserve gigabytes for
    // them before reading the first, and the peer sending this is a stranger.
    const QByteArray claim("\x80\x80\x80\x80\x01", 5);
    [[maybe_unused]] QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 20; ++i)
        EXPECT_TRUE(doc->diffSince(claim).isEmpty());
#ifndef NOTEAPPC_SANITIZED_BUILD
    EXPECT_LT(clock.elapsed(), 100) << clock.elapsed() << " ms for 20: " << "a size claim in a state vector is being believed";
#endif
    EXPECT_EQ(doc->text(), QStringLiteral("abc"));
    EXPECT_EQ(doc->stateVector(), vector);
}

TEST_P(CrdtDocTest, AnUpdateThatFailsAfterLandingInPartReportsThePart)
{
    if (GetParam() != Backend::Yrs)
        GTEST_SKIP() << "Yjs encoding; Native refuses whole";
    // Client 77: "hi" typed into the text, then a block whose parent is the
    // first block - a string, not a type - which yrs only finds out after "hi"
    // has been integrated, and reports as an error.
    const QByteArray update = QByteArray("\x01\x02\x4d\x00", 4) + QByteArray("\x04\x01\x04", 3) + "body"
        + QByteArray("\x02", 1) + "hi" + QByteArray("\x04\x00\x4d\x00\x01", 5) + "x" + QByteArray("\x00", 1);
    auto doc = make(1);
    bool ok = true;
    const QList<CrdtEdit> edits = doc->applyUpdate(update, &ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(doc->text(), QStringLiteral("hi")); // what the test stands on: part of it landed
    // ... and the editor is told, or it would show "" over a document with "hi".
    EXPECT_EQ(applyAll(QString(), edits), doc->text());
}

TEST_P(CrdtDocTest, AnEmbedFromAnotherYjsClientKeepsOffsetsInStep)
{
    if (GetParam() != Backend::Yrs)
        GTEST_SKIP() << "Yjs encoding";
    // Client 77 inserts an embed ({"a":1}) into the text, as Y.Text.insertEmbed
    // does. It occupies one offset in yrs, so the text shows one U+FFFC for it.
    const QByteArray update = QByteArray("\x01\x01\x4d\x00\x05\x01\x04", 7) + "body"
        + QByteArray("\x07", 1) + "{\"a\":1}" + QByteArray("\x00", 1);
    auto doc = make(1);
    doc->localEdit(0, 0, QStringLiteral("ab"));
    const QString mirror = applyAll(QStringLiteral("ab"), deliver(*doc, update));
    EXPECT_EQ(mirror, doc->text());
    EXPECT_EQ(doc->text().size(), 3);
    EXPECT_TRUE(doc->text().contains(QChar(0xFFFC)));
    // An edit past the embed lands where the editor thinks it does.
    ASSERT_FALSE(doc->localEdit(3, 0, QStringLiteral("z")).isEmpty());
    EXPECT_TRUE(doc->text().endsWith(QLatin1Char('z')));
    EXPECT_EQ(doc->text().size(), 4);
}

TEST_P(CrdtDocTest, AnEditWaitingBehindALaterOneIsReleased)
{
    auto base = make(1), peer = make(3), late = make(4);
    const QByteArray z = base->localEdit(0, 0, QStringLiteral("z"));
    deliver(*peer, z);
    deliver(*late, z);
    const QByteArray first = peer->localEdit(1, 0, QStringLiteral("x"));
    const QByteArray second = peer->localEdit(2, 0, QStringLiteral("y")); // typed after first's text
    const QByteArray third = peer->localEdit(0, 0, QStringLiteral("w")); // needs only z, so it lands at once

    // Delivered as third, second, first. yrs 0.28 then leaves `second` pending
    // although `first` has arrived (docs/SHARE.md, "A yrs quirk").
    deliver(*late, third);
    deliver(*late, second);
    deliver(*late, first);
    if (GetParam() == Backend::Yrs)
        catchUp(*late, *peer);
    EXPECT_EQ(late->text(), peer->text());
    EXPECT_EQ(late->stateVector(), peer->stateVector());
    EXPECT_EQ(late->text(), QStringLiteral("wzxy"));
}

namespace {

QString markdownBody(int chars)
{
    QString body;
    while (body.size() < chars)
        body += QStringLiteral("A line of markdown with a [[link]] and some **bold** text.\n");
    body.truncate(chars);
    return body;
}

/// Mean milliseconds per single-character edit at the end of a document that
/// starts `startChars` long, over `edits` edits.
double endEditCost(CrdtDoc::Backend backend, int startChars, int edits)
{
    auto doc = CrdtDoc::create(backend, 1);
    const QString body = markdownBody(startChars);
    doc->localEdit(0, 0, body);
    qsizetype end = body.size();
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < edits; ++i) {
        const QByteArray update = doc->localEdit(end, 0, QStringLiteral("x"));
        if (update.isEmpty())
            return -1;
        ++end;
    }
    return double(clock.nsecsElapsed()) / edits / 1e6;
}

} // namespace

TEST_P(CrdtDocTest, TypingAtTheEndOfAMegabyteStaysUnderTheKeystrokeBudget)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kEdits = 10000;
    const Backend backend = GetParam();
    const double mid = perf::bestOf([&] { return endEditCost(backend, 100 * 1000, kEdits); }, 1); // load only inflates it, which loosens the ratio
    const double big = perf::bestOf([&] { return endEditCost(backend, 1000 * 1000, kEdits); }, 3, 3.0);
    ASSERT_GT(mid, 0);
    ASSERT_GT(big, 0);
    std::printf("[ PERF     ] %d one-character edits at the end: 100 KB doc %.4f ms/edit, 1 MB doc %.4f ms/edit (%.1fx)\n",
                kEdits, mid, big, big / mid);
    // 10x the text may cost 10x (yrs walks the block holding the position);
    // a quadratic cost would be 100x. And one keystroke stays well inside the
    // 8 ms budget of docs/PERFORMANCE.md rule 5, at under half of it.
    EXPECT_LT(big, mid * 25 + 0.01) << "cost grows faster than the document";
    EXPECT_LT(big, 3.0);
}

namespace {

/// A random edit of `doc` that never splits an emoji.
struct RandomEdit {
    qsizetype at = 0, removed = 0;
    QString inserted;
};

RandomEdit randomEdit(const QString &text, QRandomGenerator &rng)
{
    static const QStringList pieces = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral(" "),
                                       QStringLiteral("\n"), QStringLiteral("\U0001F600")};
    RandomEdit e;
    const qsizetype size = text.size();
    // Half the edits land in the first few characters, so replicas collide at one spot.
    e.at = rng.bounded(2) ? rng.bounded(int(std::min<qsizetype>(size, 5) + 1)) : rng.bounded(int(size + 1));
    while (e.at < size && text[e.at].isLowSurrogate())
        ++e.at;
    if (size > e.at && rng.bounded(3) == 0) {
        e.removed = rng.bounded(int(std::min<qsizetype>(size - e.at, 8)) + 1);
        while (e.at + e.removed < size && text[e.at + e.removed].isLowSurrogate())
            ++e.removed;
    }
    if (e.removed == 0 || rng.bounded(2))
        for (int n = rng.bounded(5) + (e.removed == 0 ? 1 : 0); n > 0; --n)
            e.inserted += pieces[rng.bounded(int(pieces.size()))];
    return e;
}

} // namespace

TEST_P(CrdtDocTest, ThreeReplicasConvergeUnderRandomDelivery)
{
    struct Replica {
        std::unique_ptr<CrdtDoc> doc;
        QString mirror; // what an editor showing this replica holds, fed only by edits
        QList<QByteArray> inbox;
    };
    for (const quint32 seed : {1u, 2u, 3u, 4u, 5u, 20261019u}) {
        SCOPED_TRACE(QStringLiteral("seed %1").arg(seed).toStdString());
        QRandomGenerator rng(seed);
        Replica replicas[3];
        for (int i = 0; i < 3; ++i)
            replicas[i].doc = make(i + 1);

        auto deliverSome = [&](Replica &to, bool all) {
            std::shuffle(to.inbox.begin(), to.inbox.end(), rng);
            const int count = all ? int(to.inbox.size()) : rng.bounded(int(to.inbox.size()) + 1);
            for (int i = 0; i < count; ++i)
                to.mirror = applyAll(to.mirror, deliver(*to.doc, to.inbox.takeFirst()));
            ASSERT_EQ(to.mirror, to.doc->text());
        };

        for (int round = 0; round < 300; ++round) {
            const int who = rng.bounded(3);
            Replica &r = replicas[who];
            const RandomEdit e = randomEdit(r.doc->text(), rng);
            const QByteArray update = r.doc->localEdit(e.at, e.removed, e.inserted);
            r.mirror.replace(e.at, e.removed, e.inserted);
            ASSERT_EQ(r.mirror, r.doc->text()) << "round " << round;
            if (update.isEmpty())
                continue;
            for (int other = 0; other < 3; ++other)
                if (other != who)
                    replicas[other].inbox.append(update);
            for (Replica &to : replicas)
                if (rng.bounded(2)) {
                    deliverSome(to, false);
                    ASSERT_FALSE(::testing::Test::HasFatalFailure());
                }
        }
        for (Replica &to : replicas) {
            deliverSome(to, true);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }

        if (GetParam() == Backend::Yrs) {
            // yrs can keep an update pending although the edit it waited for
            // has arrived (AnEditWaitingBehindALaterOneIsReleased is the small
            // case): peers that meet exchange state vectors, and that releases
            // it. Native must not need this.
            for (Replica &to : replicas)
                for (const Replica &from : replicas)
                    to.mirror = applyAll(to.mirror, deliver(*to.doc, from.doc->diffSince(to.doc->stateVector())));
            for (const Replica &r : replicas)
                ASSERT_EQ(r.mirror, r.doc->text());
        }
        for (int i = 1; i < 3; ++i) {
            EXPECT_EQ(replicas[i].doc->text(), replicas[0].doc->text());
            EXPECT_EQ(replicas[i].doc->stateVector(), replicas[0].doc->stateVector());
        }
        EXPECT_GT(replicas[0].doc->text().size(), 20);
        if (GetParam() == Backend::Native) { // the state is a function of the items, however each replica fragmented its runs
            for (int i = 1; i < 3; ++i)
                EXPECT_EQ(replicas[i].doc->encodeState(), replicas[0].doc->encodeState());
        }
    }
}

TEST_P(CrdtDocTest, AnUpdateReferringToUnseenEditsWaits)
{
    auto a = make(1), b = make(2);
    const QByteArray first = a->localEdit(0, 0, QStringLiteral("hello"));
    const QByteArray second = a->localEdit(5, 0, QStringLiteral(" world"));
    const QByteArray third = a->localEdit(1, 2, QString()); // a deletion of text only `first` carries

    // The later updates overtake the first: kept, not applied, not refused.
    EXPECT_TRUE(deliver(*b, second).isEmpty());
    EXPECT_TRUE(deliver(*b, third).isEmpty());
    EXPECT_EQ(b->text(), QString());

    QString mirror = applyAll(QString(), deliver(*b, first));
    EXPECT_EQ(mirror, b->text());
    EXPECT_EQ(b->text(), a->text());
    EXPECT_EQ(b->text(), QStringLiteral("hlo world"));
    EXPECT_EQ(b->stateVector(), a->stateVector());

    // Not only a gap in the sender's own edits: an edit typed after another
    // replica's text waits for that text, though its own clock is next in line.
    auto c = make(3), d = make(4);
    const QByteArray base = c->localEdit(0, 0, QStringLiteral("base"));
    deliver(*d, base);
    const QByteArray reply = d->localEdit(4, 0, QStringLiteral(" reply"));
    auto e = make(5);
    EXPECT_TRUE(deliver(*e, reply).isEmpty());
    EXPECT_EQ(e->text(), QString());
    deliver(*e, base);
    EXPECT_EQ(e->text(), QStringLiteral("base reply"));
}

TEST_P(CrdtDocTest, AReloadedReplicaKeepsMergingLikeTheOthers)
{
    QRandomGenerator rng(99);
    std::unique_ptr<CrdtDoc> docs[3] = {make(1), make(2), make(3)};
    QList<QByteArray> inbox[3];
    auto deliverSome = [&](int to, bool all) {
        std::shuffle(inbox[to].begin(), inbox[to].end(), rng);
        const int count = all ? int(inbox[to].size()) : rng.bounded(int(inbox[to].size()) + 1);
        for (int i = 0; i < count; ++i)
            deliver(*docs[to], inbox[to].takeFirst());
    };
    // A long concurrent history in which every replica keeps typing after text
    // the others wrote, so its runs carry timestamps that are not simply
    // consecutive, and some of its edits were typed beside another's.
    for (int round = 0; round < 600; ++round) {
        const int who = rng.bounded(3);
        const RandomEdit e = randomEdit(docs[who]->text(), rng);
        const QByteArray update = docs[who]->localEdit(e.at, e.removed, e.inserted);
        for (int other = 0; other < 3; ++other)
            if (other != who && !update.isEmpty())
                inbox[other].append(update);
        for (int to = 0; to < 3; ++to)
            if (rng.bounded(2))
                deliverSome(to, false);
    }
    for (int to = 0; to < 3; ++to)
        deliverSome(to, true);
    if (GetParam() == Backend::Yrs) // see ThreeReplicasConvergeUnderRandomDelivery
        for (auto &to : docs)
            for (auto &from : docs)
                catchUp(*to, *from);
    ASSERT_EQ(docs[0]->text(), docs[1]->text());

    // Restarted from the stored state, then concurrent edits with a replica
    // that never restarted: the two must still end in one text.
    auto reloaded = make(4);
    ASSERT_TRUE(reloaded->loadState(docs[0]->encodeState()));
    ASSERT_EQ(reloaded->text(), docs[1]->text());
    CrdtDoc *pair[2] = {reloaded.get(), docs[1].get()};
    QList<QByteArray> toward[2];
    for (int round = 0; round < 200; ++round) {
        const int who = rng.bounded(2);
        const RandomEdit e = randomEdit(pair[who]->text(), rng);
        const QByteArray update = pair[who]->localEdit(e.at, e.removed, e.inserted);
        if (!update.isEmpty())
            toward[1 - who].append(update);
    }
    for (int to = 0; to < 2; ++to)
        for (const QByteArray &update : toward[to])
            deliver(*pair[to], update);
    EXPECT_EQ(pair[0]->text(), pair[1]->text());
}

TEST_P(CrdtDocTest, ConcurrentForwardTypingAtOneSpotDoesNotInterleave)
{
    auto a = make(1), b = make(2);
    deliver(*b, a->localEdit(0, 0, QStringLiteral("ab")));

    // Both type three characters, one edit each, into the same gap.
    QList<QByteArray> fromA, fromB;
    for (int i = 0; i < 3; ++i) {
        fromA.append(a->localEdit(1 + i, 0, QString(QLatin1Char(char('1' + i)))));
        fromB.append(b->localEdit(1 + i, 0, QString(QLatin1Char(char('x' + i)))));
    }
    for (const QByteArray &u : fromB)
        deliver(*a, u);
    for (const QByteArray &u : fromA)
        deliver(*b, u);

    EXPECT_EQ(a->text(), b->text());
    EXPECT_TRUE(a->text() == QStringLiteral("a123xyzb") || a->text() == QStringLiteral("axyz123b"))
        << qPrintable(a->text()); // backward typing at one offset may interleave; forward typing may not
}

namespace {

void putVarint(QByteArray &out, quint64 v)
{
    for (; v >= 0x80; v >>= 7)
        out.append(char(v | 0x80));
    out.append(char(v));
}

/// A Native update of `count` one-character runs, client c's run typed after
/// client c-1's, written in the order given by `descending`.
QByteArray chainUpdate(int count, bool descending)
{
    QByteArray out("N\x01", 2);
    putVarint(out, count);
    for (int k = 0; k < count; ++k) {
        const quint64 client = descending ? count - k : k + 1;
        putVarint(out, client);
        putVarint(out, 0); // clock
        putVarint(out, client); // ts
        out.append(client > 1 ? char(1) : char(0)); // origin: the previous client's character, or ROOT
        if (client > 1) {
            putVarint(out, client - 1);
            putVarint(out, 0);
        }
        putVarint(out, 1);
        putVarint(out, 1);
        out.append('x');
    }
    putVarint(out, 0); // no deletions
    return out;
}

} // namespace

TEST_P(CrdtDocTest, MalformedUpdatesWithAValidHeaderAreCheap)
{
    if (GetParam() != Backend::Native)
        GTEST_SKIP() << "Native's own format";
    QRandomGenerator rng(7);
    [[maybe_unused]] QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 20000; ++i) {
        QByteArray payload("N\x01", 2);
        for (int n = rng.bounded(65); n > 0; --n) {
            const quint32 pick = rng.bounded(3);
            payload.append(char(pick == 0 ? rng.bounded(8) : pick == 1 ? rng.bounded(256) : 0x80 | rng.bounded(128)));
        }
        auto victim = make(100);
        victim->localEdit(0, 0, QStringLiteral("x"));
        const QString text = victim->text();
        const QByteArray vector = victim->stateVector();
        bool ok = true;
        victim->applyUpdate(payload, &ok);
        if (!ok) {
            ASSERT_EQ(victim->text(), text);
            ASSERT_EQ(victim->stateVector(), vector);
        }
    }
#ifndef NOTEAPPC_SANITIZED_BUILD
    EXPECT_LT(clock.elapsed(), 2000) << "a size claim in a bad update is being believed";
#endif
}

TEST_P(CrdtDocTest, PendingIsBounded)
{
    if (GetParam() != Backend::Native)
        GTEST_SKIP() << "Native's own limit";
    auto victim = make(1);
    victim->localEdit(0, 0, QStringLiteral("mine"));
    const QString text = victim->text();
    const QByteArray vector = victim->stateVector();
    const QString big(2'000'000, QLatin1Char('z'));

    // Each update types a big run after a character the victim never saw, so
    // each one waits; a peer that keeps sending them must hit a ceiling.
    int refusedAt = -1;
    for (int i = 0; i < 60 && refusedAt < 0; ++i) {
        auto source = make(1000 + i);
        source->localEdit(0, 0, QStringLiteral("o"));
        const QByteArray update = source->localEdit(1, 0, big);
        bool ok = true;
        EXPECT_TRUE(victim->applyUpdate(update, &ok).isEmpty());
        if (!ok)
            refusedAt = i;
        EXPECT_EQ(victim->text(), text);
        EXPECT_EQ(victim->stateVector(), vector);
    }
    EXPECT_GT(refusedAt, 5) << "refused far too early";
    EXPECT_GE(refusedAt, 0) << "never refused: pending grows without bound";
}

TEST_P(CrdtDocTest, AHostileDependencyChainIsLinear)
{
    if (GetParam() != Backend::Native)
        GTEST_SKIP() << "Native's own format";
    constexpr int kRuns = 100000;
    // Every run waits for the one after it in the update, so nothing can be
    // placed until the last arrives, and then all of them can: one pass per
    // run would be quadratic.
    [[maybe_unused]] QElapsedTimer clock;
    clock.start();
    auto doc = make(1);
    bool ok = false;
    doc->applyUpdate(chainUpdate(kRuns, true), &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(doc->text().size(), kRuns);
#ifndef NOTEAPPC_SANITIZED_BUILD
    EXPECT_LT(clock.elapsed(), 3000);
#endif
    auto ordered = make(2);
    ordered->applyUpdate(chainUpdate(kRuns, false), &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(ordered->encodeState(), doc->encodeState());
}

namespace {

/// Bytes the allocator hands out right now, where the platform can say.
qsizetype heapInUse()
{
#ifdef __GLIBC__
    const struct mallinfo2 info = mallinfo2();
    return qsizetype(info.uordblks + info.hblkhd);
#else
    return -1;
#endif
}

} // namespace

/// Prints one line per backend; read side by side they are the comparison in
/// docs/PERFORMANCE.md. Nothing is asserted beyond sanity.
TEST_P(CrdtDocTest, SizesAndMemoryAtAMegabyte)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const Backend backend = GetParam();
    const QString body = markdownBody(1000 * 1000);

    // Bytes: one keystroke, the whole note, a diff after scattered edits.
    auto a = CrdtDoc::create(backend, 1), peer = CrdtDoc::create(backend, 2);
    a->localEdit(0, 0, body);
    const qsizetype keystroke = a->localEdit(body.size(), 0, QStringLiteral("x")).size();
    const QByteArray state = a->encodeState();
    ASSERT_TRUE(peer->loadState(state));
    QRandomGenerator rng(11);
    for (int i = 0; i < 100; ++i)
        a->localEdit(rng.bounded(int(a->text().size()) + 1), 0, QStringLiteral("y"));
    const qsizetype diff = a->diffSince(peer->stateVector()).size();

    // Cost per edit at random positions: splits and moves the position around.
    // The text is ASCII, so no offset splits a surrogate pair.
    constexpr int kRandomEdits = 2000;
    QElapsedTimer clock;
    clock.start();
    qsizetype size = a->text().size();
    for (int i = 0; i < kRandomEdits; ++i) {
        if (a->localEdit(rng.bounded(int(size) + 1), 0, QStringLiteral("z")).isEmpty())
            FAIL() << "edit refused";
        ++size;
    }
    const double randomCost = double(clock.nsecsElapsed()) / kRandomEdits / 1e6;

    // Cost of taking in one remote one-character edit at a random position,
    // into the note as fragmented by those edits (what a collaborator costs).
    ASSERT_TRUE(peer->loadState(a->encodeState()));
    constexpr int kRemoteEdits = 500;
    QList<QByteArray> remote;
    for (int i = 0; i < kRemoteEdits; ++i)
        remote.append(a->localEdit(rng.bounded(int(size) + 1), 0, QStringLiteral("w")));
    clock.restart();
    for (const QByteArray &update : remote)
        EXPECT_FALSE(peer->applyUpdate(update).isEmpty());
    const double remoteCost = double(clock.nsecsElapsed()) / kRemoteEdits / 1e6;

    // Heap for the same note built by 10 000 appends of 100 characters.
    const qsizetype heapBefore = heapInUse();
    {
        auto built = CrdtDoc::create(backend, 3);
        const QString chunk = body.left(100);
        qsizetype end = 0;
        for (int i = 0; i < 10000; ++i) {
            built->localEdit(end, 0, chunk);
            end += chunk.size();
        }
        const qsizetype heap = heapInUse() - heapBefore;
        std::printf("[ BENCH    ] %-6s 1 MB: random-position edit %.4f ms (%d edits), remote edit applied %.4f ms (%d edits), "
                    "keystroke update %lld B, encodeState %lld B, diff after 100 scattered edits %lld B, "
                    "heap for 10 000 appends %lld KB\n",
                    backend == Backend::Native ? "Native" : "Yrs", randomCost, kRandomEdits, remoteCost, kRemoteEdits,
                    qint64(keystroke), qint64(state.size()), qint64(diff), qint64(heap / 1024));
        EXPECT_EQ(built->text().size(), end);
    }
    EXPECT_GT(keystroke, 0);
    EXPECT_GT(state.size(), 1000 * 1000);
    EXPECT_GT(diff, 0);
}

// Positions that stay on their characters (the carets a share draws): a
// position is a few bytes naming the character before it, so every replica
// that has seen the character puts it in the same place whatever else it has
// seen, in whatever order.
TEST_P(CrdtDocTest, AnEncodedPositionStaysOnItsCharacterThroughAThirdMembersEdits)
{
    std::unique_ptr<CrdtDoc> docs[3] = {make(1), make(2), make(3)};
    CrdtDoc &a = *docs[0], &b = *docs[1], &c = *docs[2];
    const QByteArray start = a.localEdit(0, 0, QStringLiteral("hello world"));
    deliver(b, start);
    deliver(c, start);

    const QByteArray beforeW = a.encodePosition(6);  // "hello |world"
    const QByteArray afterWo = b.encodePosition(8);  // "hello wo|rld"
    ASSERT_FALSE(beforeW.isEmpty());
    ASSERT_FALSE(afterWo.isEmpty());
    const auto expectAll = [&](qsizetype first, qsizetype second, const char *when) {
        for (const auto &doc : docs) {
            EXPECT_EQ(doc->resolvePosition(beforeW), first) << when;
            EXPECT_EQ(doc->resolvePosition(afterWo), second) << when;
        }
    };
    expectAll(6, 8, "untouched");

    const QByteArray insert = c.localEdit(0, 0, QStringLiteral(">> ")); // before both
    deliver(a, insert);
    // b has not heard of it yet: its own view is unchanged, a's and c's moved
    EXPECT_EQ(b.resolvePosition(beforeW), 6);
    EXPECT_EQ(a.resolvePosition(beforeW), 9);
    EXPECT_EQ(c.resolvePosition(afterWo), 11);
    deliver(b, insert);
    expectAll(9, 11, "after c typed before them");

    const QByteArray erase = c.localEdit(3, 6, QString()); // ">> world": the first caret's character is gone
    deliver(a, erase);
    deliver(b, erase);
    EXPECT_EQ(c.text(), QStringLiteral(">> world"));
    expectAll(3, 5, "after c deleted around the first and before the second");

    // Typing at the position itself leaves it where it was, ahead of the new text
    deliver(b, a.localEdit(5, 0, QStringLiteral("XY")));
    EXPECT_EQ(b.resolvePosition(afterWo), 5);
    EXPECT_EQ(b.text(), QStringLiteral(">> woXYrld"));
}

TEST_P(CrdtDocTest, PositionsAreTheSameWhenEditsArriveInAnotherOrder)
{
    auto a = make(1), b = make(2), c = make(3);
    const QByteArray start = a->localEdit(0, 0, QStringLiteral("0123456789"));
    deliver(*b, start);
    deliver(*c, start);
    const QByteArray atFive = a->encodePosition(5);
    const QByteArray fromB = b->localEdit(2, 0, QStringLiteral("bb"));
    const QByteArray fromC = c->localEdit(3, 4, QStringLiteral("c"));
    // a hears b then c, b hears c, c hears b: three orders of the same two edits
    deliver(*a, fromB);
    deliver(*a, fromC);
    deliver(*b, fromC);
    deliver(*c, fromB);
    ASSERT_EQ(a->text(), b->text());
    ASSERT_EQ(a->text(), c->text());
    EXPECT_EQ(a->resolvePosition(atFive), b->resolvePosition(atFive));
    EXPECT_EQ(a->resolvePosition(atFive), c->resolvePosition(atFive));
    // (position 5 followed the '4' that c deleted; where it closes up is the backend's call, but it is one place)
    EXPECT_GE(a->resolvePosition(atFive), 0);
    EXPECT_LE(a->resolvePosition(atFive), a->text().size());
}

TEST_P(CrdtDocTest, PositionsAtTheEdgesAndWhenNotOnOffer)
{
    auto a = make(1), b = make(2);
    EXPECT_EQ(a->resolvePosition(a->encodePosition(0)), 0) << "an empty note has a start";
    deliver(*b, a->localEdit(0, 0, QStringLiteral("abc")));
    const QByteArray start = b->encodePosition(0), end = b->encodePosition(3);
    ASSERT_FALSE(start.isEmpty());
    ASSERT_FALSE(end.isEmpty());
    deliver(*b, a->localEdit(0, 0, QStringLiteral("<")));
    deliver(*b, a->localEdit(4, 0, QStringLiteral(">")));
    EXPECT_EQ(b->resolvePosition(start), 0) << "the start stays the start";
    EXPECT_EQ(b->resolvePosition(end), 4) << "after the last 'c', before what was typed there";
    EXPECT_TRUE(b->encodePosition(-1).isEmpty());
    EXPECT_TRUE(b->encodePosition(b->text().size() + 1).isEmpty());

    // A character this replica has not seen: no answer, the caller falls back
    auto c = make(3);
    const QByteArray typed = c->localEdit(0, 0, QStringLiteral("zz"));
    const QByteArray unseen = c->encodePosition(2);
    EXPECT_EQ(b->resolvePosition(unseen), -1);
    deliver(*b, typed);
    EXPECT_EQ(b->resolvePosition(unseen), b->text().indexOf(QStringLiteral("zz")) + 2);
}

TEST_P(CrdtDocTest, GarbagePositionsAreRefusedNotTrusted)
{
    auto doc = make(1);
    doc->localEdit(0, 0, QStringLiteral("hello"));
    EXPECT_EQ(doc->resolvePosition({}), -1);
    EXPECT_EQ(doc->resolvePosition(QByteArray("\x00\x01\x02", 3)), -1);
    EXPECT_EQ(doc->resolvePosition(QByteArray(100000, '\xff')), -1);
    QRandomGenerator rng(7);
    for (int i = 0; i < 2000; ++i) {
        QByteArray noise(int(rng.bounded(1, 40)), 0);
        for (char &ch : noise)
            ch = char(rng.bounded(256));
        const qsizetype at = doc->resolvePosition(noise);
        EXPECT_GE(at, -1);
        EXPECT_LE(at, doc->text().size());
    }
    QByteArray good = doc->encodePosition(3);
    ASSERT_FALSE(good.isEmpty());
    for (qsizetype cut = 0; cut < good.size(); ++cut)
        EXPECT_EQ(doc->resolvePosition(good.left(cut)), -1) << "cut at " << cut;
    EXPECT_EQ(doc->resolvePosition(good + "x"), -1) << "nothing may trail";
}

TEST_P(CrdtDocTest, PositionsFollowTheirCharactersUnderRandomEdits)
{
    std::unique_ptr<CrdtDoc> docs[3] = {make(11), make(22), make(33)};
    QRandomGenerator rng(2026);
    const auto broadcast = [&](int from, const QByteArray &update) {
        for (int i = 0; i < 3; ++i)
            if (i != from)
                deliver(*docs[i], update);
    };
    broadcast(0, docs[0]->localEdit(0, 0, QStringLiteral("the quick brown fox jumps over the lazy dog")));
    for (int round = 0; round < 300; ++round) {
        // a position somebody holds ...
        const int owner = int(rng.bounded(3));
        const qsizetype offset = rng.bounded(qsizetype(docs[owner]->text().size()) + 1);
        const QByteArray pos = docs[owner]->encodePosition(offset);
        ASSERT_FALSE(pos.isEmpty());
        // ... and another member's edit, which every replica then has
        const int editor = int((owner + 1 + rng.bounded(2)) % 3);
        const qsizetype size = docs[editor]->text().size();
        const qsizetype at = rng.bounded(size + 1);
        const bool erase = size > at && rng.bounded(2);
        const qsizetype removed = erase ? rng.bounded(qsizetype(1), qMin<qsizetype>(6, size - at) + 1) : 0;
        const QString inserted = erase ? QString() : QStringLiteral("ab").left(1 + rng.bounded(2));
        broadcast(editor, docs[editor]->localEdit(at, removed, inserted));
        qsizetype expected = offset;
        if (erase) {
            if (offset >= at + removed)
                expected = offset - removed;
            else if (offset > at)
                expected = at; // its character is gone: the position closes up on the hole
        } else if (offset > at) {
            expected = offset + inserted.size(); // typed at the position itself: it stays ahead of the new text
        }
        for (const auto &doc : docs)
            ASSERT_EQ(doc->resolvePosition(pos), expected)
                << "round " << round << " offset " << offset << " edit at " << at << " removed " << removed
                << " inserted " << inserted.size();
        ASSERT_EQ(docs[0]->text(), docs[1]->text());
        ASSERT_EQ(docs[0]->text(), docs[2]->text());
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, CrdtDocTest, ::testing::Values(Backend::Yrs, Backend::Native),
                         [](const ::testing::TestParamInfo<Backend> &info) {
                             return info.param == Backend::Yrs ? "Yrs" : "Native";
                         });
