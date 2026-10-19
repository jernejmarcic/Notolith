// CrdtDoc (libs/ext/share): one suite, run over every backend. A backend the
// build lacks skips; see docs/SHARE.md.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <cstdio>

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

namespace {

/// Mean milliseconds per single-character edit at the end of a document that
/// starts `startChars` long, over `edits` edits.
double endEditCost(CrdtDoc::Backend backend, int startChars, int edits)
{
    auto doc = CrdtDoc::create(backend, 1);
    QString body;
    while (body.size() < startChars)
        body += QStringLiteral("A line of markdown with a [[link]] and some **bold** text.\n");
    body.truncate(startChars);
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

INSTANTIATE_TEST_SUITE_P(Backends, CrdtDocTest, ::testing::Values(Backend::Yrs, Backend::Native),
                         [](const ::testing::TestParamInfo<Backend> &info) {
                             return info.param == Backend::Yrs ? "Yrs" : "Native";
                         });
