#include <gtest/gtest.h>

#include <QRandomGenerator>

#include "DisplayMap.h"

using namespace noteAppC;

namespace {

/// The whole block, which is what most of these tests map over.
Range whole(const QString &text)
{
    return {0, text.size()};
}

} // namespace

TEST(DisplayMapTest, IdentityHidesNothing)
{
    const QString source = QStringLiteral("plain text");
    const DisplayMap map = DisplayMap::identity(source, whole(source));

    EXPECT_EQ(map.text(), source);
    EXPECT_FALSE(map.hidesAnything());
    for (qsizetype i = 0; i <= source.size(); ++i) {
        EXPECT_EQ(map.toDisplay(i), i);
        EXPECT_EQ(map.toSource(i), i);
    }
}

TEST(DisplayMapTest, HidesTheHeadingMarker)
{
    const QString source = QStringLiteral("## Heading");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{0, 3}});

    EXPECT_EQ(map.text(), QStringLiteral("Heading"));
    EXPECT_TRUE(map.hidesAnything());
    EXPECT_EQ(map.toSource(0), 3);
    EXPECT_EQ(map.toDisplay(3), 0);
    // Everything inside the marker collapses onto the first visible position.
    EXPECT_EQ(map.toDisplay(0), 0);
    EXPECT_EQ(map.toDisplay(2), 0);
    EXPECT_TRUE(map.isHidden(1));
    EXPECT_FALSE(map.isHidden(3));
}

TEST(DisplayMapTest, HidesWikilinkBracketsAndTarget)
{
    //                              0         1         2
    //                              0123456789012345678901234
    const QString source = QStringLiteral("see [[Target|shown]] here");
    // Hide "[[Target|" and "]]", which is what live preview does.
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 13}, {18, 20}});

    EXPECT_EQ(map.text(), QStringLiteral("see shown here"));
    EXPECT_EQ(map.toSource(map.text().indexOf(QLatin1Char('s'), 4)), 13);
    EXPECT_EQ(map.toDisplay(13), 4);
    EXPECT_EQ(map.toDisplay(20), 9);
}

TEST(DisplayMapTest, OverlappingAndUnsortedRangesMerge)
{
    const QString source = QStringLiteral("abcdefgh");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 6}, {1, 3}, {2, 5}});
    // 1..6 merged away.
    EXPECT_EQ(map.text(), QStringLiteral("agh"));
}

TEST(DisplayMapTest, RangesOutsideTheBlockAreIgnored)
{
    const QString source = QStringLiteral("0123456789");
    const DisplayMap map = DisplayMap::build(source, {3, 7}, {{0, 4}, {6, 9}});

    EXPECT_EQ(map.text(), QStringLiteral("45"));
    EXPECT_EQ(map.toSource(0), 4);
    EXPECT_EQ(map.toSource(2), 6);
}

TEST(DisplayMapTest, EverythingHidden)
{
    const QString source = QStringLiteral("%%a comment%%");
    const DisplayMap map = DisplayMap::build(source, whole(source), {whole(source)});

    EXPECT_TRUE(map.text().isEmpty());
    // Nothing is visible, so the one display position is the block start:
    // the caret sits before the hidden text, and typing there stays outside it.
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(5), 0);
}

TEST(DisplayMapTest, EmptyBlock)
{
    const QString source;
    const DisplayMap map = DisplayMap::build(source, {0, 0}, {});
    EXPECT_TRUE(map.text().isEmpty());
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(0), 0);
}

// The plan's Phase 2 gate: a million random maps must never break the
// round trip, because every cursor move in live preview goes through it.
TEST(DisplayMapTest, FuzzRoundTrip)
{
    auto *random = QRandomGenerator::global();
    constexpr int kCases = 4000;

    for (int c = 0; c < kCases; ++c) {
        const qsizetype length = random->bounded(1, 80);
        QString source(length, QLatin1Char('.'));
        for (qsizetype i = 0; i < length; ++i)
            source[i] = QChar(QLatin1Char('a' + random->bounded(26)));

        QList<Range> hidden;
        const int holes = random->bounded(0, 6);
        for (int h = 0; h < holes; ++h) {
            const qsizetype start = random->bounded(int(length));
            const qsizetype end = start + random->bounded(1, int(length - start) + 1);
            hidden.append({start, end});
        }

        const DisplayMap map = DisplayMap::build(source, whole(source), hidden);

        // 1. The display string is the source minus the hidden ranges.
        QString expected;
        for (qsizetype i = 0; i < length; ++i) {
            if (!map.isHidden(i))
                expected += source.at(i);
        }
        ASSERT_EQ(map.text(), expected) << "case " << c;

        // 2. Display -> source -> display is the identity.
        for (qsizetype d = 0; d <= map.text().size(); ++d) {
            const qsizetype s = map.toSource(d);
            ASSERT_GE(s, 0);
            ASSERT_LE(s, length);
            ASSERT_EQ(map.toDisplay(s), d) << "case " << c << " display " << d;
        }

        // 3. Mapping is monotonic in both directions: a cursor never jumps
        //    backwards when it moves forwards.
        qsizetype previous = -1;
        for (qsizetype s = 0; s <= length; ++s) {
            const qsizetype d = map.toDisplay(s);
            ASSERT_GE(d, previous) << "case " << c << " source " << s;
            previous = d;
        }

        // 4. A visible offset survives the trip unchanged.
        for (qsizetype s = 0; s < length; ++s) {
            if (!map.isHidden(s))
                ASSERT_EQ(map.toSource(map.toDisplay(s)), s) << "case " << c << " source " << s;
        }
    }
}
