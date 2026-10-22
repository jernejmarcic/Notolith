// The handwriting-font glyph studio's data layer (libs/ext/hwfont): the glyph
// set, the on-disk names and the Handwriting/<font>/<hex>/<n>.ink store.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#include "GlyphSet.h"
#include "GlyphStore.h"
#include "InkDocument.h"
#include "InkFormat.h"

using namespace noteAppC::hwfont;
using noteAppC::ink::InkDocument;
using noteAppC::ink::Stroke;

namespace {

const GlyphTarget *find(const std::vector<GlyphTarget> &set, char32_t codepoint)
{
    const auto it = std::find_if(set.begin(), set.end(), [codepoint](const GlyphTarget &t) { return t.codepoint == codepoint; });
    return it == set.end() ? nullptr : &*it;
}

InkDocument sampleWithOneStroke()
{
    InkDocument sample;
    sample.setSettings(GlyphStore::samplePage());
    auto stroke = std::make_shared<Stroke>();
    stroke->points = {{50, 100, 0.5}, {150, 280, 0.5}};
    sample.addStroke(stroke);
    sample.commit();
    return sample;
}

} // namespace

TEST(GlyphSetTest, DefaultSetHasLettersDigitsPunctuationAndSlovene)
{
    const auto set = defaultGlyphSet(3, 1);
    for (const char32_t c : {U'a', U'Z', U'0', U'!', U'č', U'š', U'ž', U'Č', U'Š', U'Ž'})
        EXPECT_NE(find(set, c), nullptr) << "missing U+" << std::hex << uint32_t(c);
    EXPECT_EQ(find(set, U'a')->wanted, 3);
    EXPECT_EQ(find(set, U'č')->wanted, 3);
    EXPECT_EQ(find(set, U'!')->wanted, 1);
    EXPECT_EQ(find(set, U'0')->wanted, 1);
    EXPECT_TRUE(std::is_sorted(set.begin(), set.end(), [](const GlyphTarget &a, const GlyphTarget &b) { return a.codepoint < b.codepoint; }));
    EXPECT_EQ(std::adjacent_find(set.begin(), set.end(), [](const GlyphTarget &a, const GlyphTarget &b) { return a.codepoint == b.codepoint; }), set.end());
}

TEST(GlyphSetTest, DirectoryNamesRoundTrip)
{
    EXPECT_EQ(glyphDirectoryName(U'a'), QStringLiteral("0061"));
    EXPECT_EQ(glyphDirectoryName(U'č'), QStringLiteral("010d"));
    EXPECT_EQ(glyphDirectoryName(0x1F600), QStringLiteral("01f600"));
    char32_t out = 0;
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("0061"), &out));
    EXPECT_EQ(out, U'a');
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("010d"), &out));
    EXPECT_EQ(out, U'č');
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("01f600"), &out));
    EXPECT_EQ(out, char32_t(0x1F600));
    for (const char *bad : {"", "61", "010D", "00061", "zzzz", "0061 ", "..", "110000", "-061"})
        EXPECT_FALSE(codepointFromDirectoryName(QString::fromLatin1(bad), &out)) << bad;
}

TEST(GlyphStoreTest, SamplePageIsUsableAndFixed)
{
    const auto page = GlyphStore::samplePage();
    EXPECT_TRUE(page.isUsable());
    EXPECT_EQ(page.layout, noteAppC::ink::LayoutKind::Fixed);
    EXPECT_DOUBLE_EQ(page.format.width, 400);
    EXPECT_DOUBLE_EQ(page.format.height, 400);
    EXPECT_EQ(page.background.pattern, noteAppC::ink::Pattern::None);
    EXPECT_EQ(page.background.color, QColor(Qt::white));
    const auto m = GlyphStore::defaultMetrics();
    EXPECT_DOUBLE_EQ(m.baseline, 280);
    EXPECT_DOUBLE_EQ(m.xHeight, 160);
    EXPECT_DOUBLE_EQ(m.ascender, 100);
}

TEST(GlyphStoreTest, SavesUnderHandwritingFontHexN)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const InkDocument sample = sampleWithOneStroke();
    const quint64 revision = sample.revision();
    const bool modified = sample.isModified();

    QString error;
    ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    const QString dir = vault.path() + QStringLiteral("/Handwriting/Test/0061");
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/1.ink")));
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/2.ink")));
    EXPECT_EQ(store.sampleCount(U'a'), 2);
    EXPECT_EQ(store.sampleCount(U'b'), 0);
    EXPECT_EQ(store.nextSamplePath(U'a'), dir + QStringLiteral("/3.ink"));
    EXPECT_EQ(store.fontDirectory(), vault.path() + QStringLiteral("/Handwriting/Test"));

    QFile file(dir + QStringLiteral("/1.ink"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_TRUE(bytes.contains("\"glyph\":{")) << bytes.constData();
    EXPECT_TRUE(bytes.contains("\"codepoint\":97")) << bytes.constData();
    InkDocument back;
    ASSERT_TRUE(noteAppC::ink::readInk(bytes, back).ok);
    const QJsonObject glyph = back.settings().extra.value(QStringLiteral("glyph")).toObject();
    EXPECT_EQ(glyph.value(QStringLiteral("codepoint")).toInt(), 97);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("baseline")).toDouble(), 280);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("xHeight")).toDouble(), 160);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("ascender")).toDouble(), 100);
    EXPECT_EQ(back.items().size(), 1u) << "the stroke is saved with its glyph";

    EXPECT_FALSE(sample.settings().extra.contains(QStringLiteral("glyph"))) << "the argument is unchanged";
    EXPECT_EQ(sample.revision(), revision);
    EXPECT_EQ(sample.isModified(), modified);
}

TEST(GlyphStoreTest, SaveCreatesTheMissingDirectories)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    EXPECT_FALSE(QFileInfo::exists(vault.path() + QStringLiteral("/Handwriting")));
    GlyphStore store(vault.path(), QStringLiteral("Hand"));
    QString error;
    ASSERT_TRUE(store.saveSample(U'č', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    EXPECT_TRUE(QFileInfo(vault.path() + QStringLiteral("/Handwriting/Hand/010d/1.ink")).isFile());
}

TEST(GlyphStoreTest, RefusesFontNamesThatCouldLeaveHandwriting)
{
    for (const QString &name : {QString(), QStringLiteral("a/b"), QStringLiteral("a\\b"), QStringLiteral(".."), QStringLiteral("a..b"),
                                QStringLiteral(".hidden"), QStringLiteral("."), QStringLiteral("../x"), QStringLiteral("bad\nname"),
                                QStringLiteral("tab\there"), QString(QChar(0)) + QStringLiteral("x"), QStringLiteral("/etc")}) {
        EXPECT_FALSE(GlyphStore::isValidFontName(name)) << qPrintable(name.toHtmlEscaped());
        QTemporaryDir vault;
        ASSERT_TRUE(vault.isValid());
        GlyphStore store(vault.path(), name);
        QString error;
        EXPECT_FALSE(store.saveSample(U'a', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error));
        EXPECT_FALSE(error.isEmpty());
        EXPECT_EQ(store.sampleCount(U'a'), 0);
        EXPECT_TRUE(store.nextSamplePath(U'a').isEmpty());
        EXPECT_TRUE(QDir(vault.path()).isEmpty()) << "nothing was written for " << qPrintable(name.toHtmlEscaped());
    }
    for (const QString &name : {QStringLiteral("Test"), QStringLiteral("My hand 2"), QStringLiteral("Piši")})
        EXPECT_TRUE(GlyphStore::isValidFontName(name)) << qPrintable(name);
}

TEST(GlyphStoreTest, DoesNotWriteThroughASymlinkedFolder)
{
    QTemporaryDir vault, outside;
    ASSERT_TRUE(vault.isValid() && outside.isValid());
    ASSERT_TRUE(QFile::link(outside.path(), vault.path() + QStringLiteral("/Handwriting")));
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    QString error;
    EXPECT_FALSE(store.saveSample(U'a', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error));
    EXPECT_TRUE(QDir(outside.path()).isEmpty()) << "nothing was written outside the vault";
}

TEST(GlyphStoreTest, ProgressPutsNeedsMoreFirst)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const InkDocument sample = sampleWithOneStroke();
    QString error;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'b', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'!', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);

    const std::vector<GlyphTarget> set{{U'!', 1}, {U'b', 3}, {U'a', 3}, {U'c', 3}};
    const auto progress = store.progress(set);
    ASSERT_EQ(progress.size(), 4u);
    // needs more: b (1 of 3), c (0 of 3), by code point; then the finished: '!' (1 of 1), a (3 of 3)
    EXPECT_EQ(progress[0].first.codepoint, U'b');
    EXPECT_EQ(progress[0].second, 1);
    EXPECT_EQ(progress[1].first.codepoint, U'c');
    EXPECT_EQ(progress[1].second, 0);
    EXPECT_EQ(progress[2].first.codepoint, U'!');
    EXPECT_EQ(progress[3].first.codepoint, U'a');
    EXPECT_EQ(progress[3].second, 3);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
