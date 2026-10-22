#include <gtest/gtest.h>

#include <zlib.h>

#include "InkTextBoxes.h"

namespace {

QByteArray gzipped(const QByteArray &plain)
{
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&stream, uLong(plain.size()))) + 32, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    stream.avail_in = uInt(plain.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    deflate(&stream, Z_FINISH);
    out.truncate(int(stream.total_out));
    deflateEnd(&stream);
    return out;
}

const QByteArray kFile = R"({"format":"notolith-ink","version":1,"document":{},"items":[)"
                         R"({"type":"stroke","t":1,"layer":"user","points":[0,0,0.5,5,5,0.5]},)"
                         R"({"type":"textbox","t":2,"layer":"user","rect":[10,10,200,40],"markdown":"See [[Lecture 3]] #exam"},)"
                         R"({"type":"shape","t":3,"layer":"user","kind":"line","points":[0,0,9,9]},)"
                         R"({"type":"textbox","t":4,"layer":"user","rect":[10,80,200,40],"markdown":"second box"}]})";

} // namespace

TEST(InkTextBoxesTest, JoinsEveryTextBoxInFileOrderAndSkipsEverythingElse)
{
    const auto text = noteAppC::inkTextBoxes(kFile);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, QStringLiteral("See [[Lecture 3]] #exam\n\nsecond box"));
}

TEST(InkTextBoxesTest, ReadsAGzipFileLikeAPlainOne)
{
    EXPECT_EQ(noteAppC::inkTextBoxes(gzipped(kFile)), noteAppC::inkTextBoxes(kFile));
    ASSERT_TRUE(noteAppC::inkTextBoxes(gzipped(kFile)).has_value());
}

TEST(InkTextBoxesTest, AFileWithoutTextBoxesIsANoteWithNoText)
{
    const auto text = noteAppC::inkTextBoxes(R"({"format":"notolith-ink","version":1,"document":{},"items":[]})");
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(text->isEmpty());
}

TEST(InkTextBoxesTest, TruncatedForeignAndMalformedInputIsNotAnInkNote)
{
    EXPECT_FALSE(noteAppC::inkTextBoxes(kFile.left(kFile.size() / 2)).has_value()) << "cut mid-write by a sync client";
    EXPECT_FALSE(noteAppC::inkTextBoxes(gzipped(kFile).left(20)).has_value()) << "a truncated gzip member";
    EXPECT_FALSE(noteAppC::inkTextBoxes(R"({"format":"something-else","items":[{"type":"textbox","markdown":"x"}]})").has_value());
    EXPECT_FALSE(noteAppC::inkTextBoxes("[1,2,3]").has_value());
    EXPECT_FALSE(noteAppC::inkTextBoxes(QByteArray()).has_value());
}

TEST(InkTextBoxesTest, ItemsOfOtherShapesAreSkippedNotFatal)
{
    const auto text = noteAppC::inkTextBoxes(
        R"({"format":"notolith-ink","version":2,"items":[7,"x",{"type":"textbox"},{"type":"textbox","markdown":5},)"
        R"({"type":"textbox","markdown":"kept","future":true}]})");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, QStringLiteral("kept"));
}

// The reader runs on every .ink file the indexer meets, so a hostile file must stay cheap and bounded.
TEST(InkTextBoxesTest, AGzipBombIsRefusedAndHugeTextIsCapped)
{
    // 80 MiB of zeros inflates past the 64 MiB cap: refused, not decoded.
    EXPECT_FALSE(noteAppC::inkTextBoxes(gzipped(QByteArray(80 << 20, '\0'))).has_value());

    // A text box of several MiB is read, but the note's text stops at the cap.
    const QByteArray big(8 << 20, 'a');
    const QByteArray file = R"({"format":"notolith-ink","version":1,"items":[{"type":"textbox","markdown":")" + big + R"("}]})";
    const auto text = noteAppC::inkTextBoxes(file);
    ASSERT_TRUE(text.has_value());
    EXPECT_LT(text->size(), big.size());
    EXPECT_GT(text->size(), 0);
}

TEST(InkTextBoxesTest, DeeplyNestedJSONDoesNotCrash)
{
    const QByteArray nested = QByteArray(100000, '[') + QByteArray(100000, ']');
    EXPECT_FALSE(noteAppC::inkTextBoxes(nested).has_value());
    noteAppC::inkTextBoxes(R"({"format":"notolith-ink","items":)" + nested + "}"); // either answer is fine: it must not crash
}
