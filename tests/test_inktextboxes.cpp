// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
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

// The index-side cap (16 MiB in, 16 MiB inflated); inkcore's own 64 MiB limit is for opening one file.
constexpr qint64 kIndexCap = qint64(16) << 20;

// Valid .ink JSON padded with one long string, so only the size cap (not the JSON parser) can refuse it.
QByteArray paddedFile(qint64 padBytes)
{
    return R"({"format":"notolith-ink","version":1,"items":[],"pad":")" + QByteArray(int(padBytes), 'a') + R"("})";
}

// The reader runs on every .ink file the indexer meets, so a hostile file must stay cheap and bounded.
TEST(InkTextBoxesTest, AGzipBombIsRefusedAtTheInflateCap)
{
    const QByteArray bomb = gzipped(paddedFile(kIndexCap + (1 << 20)));
    ASSERT_LT(bomb.size(), 1 << 20) << "a small file that inflates past the cap";
    EXPECT_FALSE(noteAppC::inkTextBoxes(bomb).has_value());
    EXPECT_TRUE(noteAppC::inkTextBoxes(gzipped(paddedFile(kIndexCap - (1 << 20)))).has_value()) << "just under the cap is fine";
}

TEST(InkTextBoxesTest, PlainJSONOverTheCapIsRefused)
{
    EXPECT_FALSE(noteAppC::inkTextBoxes(paddedFile(kIndexCap + 1)).has_value());
    EXPECT_TRUE(noteAppC::inkTextBoxes(paddedFile(kIndexCap - (1 << 20))).has_value());
}

TEST(InkTextBoxesTest, HugeTextIsCapped)
{
    const QByteArray big(8 << 20, 'a');
    const QByteArray file = R"({"format":"notolith-ink","version":1,"items":[{"type":"textbox","markdown":")" + big + R"("}]})";
    const auto text = noteAppC::inkTextBoxes(file);
    ASSERT_TRUE(text.has_value());
    EXPECT_LT(text->size(), big.size());
    EXPECT_GT(text->size(), 0);
}

TEST(InkTextBoxesTest, TheTextCapNeverLeavesALoneSurrogate)
{
    // An emoji (a surrogate pair in a QString) straddles the cap: the first half must not survive on its own.
    const QByteArray file = R"({"format":"notolith-ink","version":1,"items":[{"type":"textbox","markdown":")"
                            + QByteArray((1 << 20) - 1, 'a') + "\xF0\x9F\x98\x80" + R"("}]})";
    const auto text = noteAppC::inkTextBoxes(file);
    ASSERT_TRUE(text.has_value());
    ASSERT_FALSE(text->isEmpty());
    EXPECT_FALSE(text->back().isHighSurrogate());
}

// inkcore's readInk refuses these; a file that shows in search but cannot be opened would be worse than a missing one.
TEST(InkTextBoxesTest, TrailingDataAfterGzipAndAMissingVersionAreRefused)
{
    EXPECT_FALSE(noteAppC::inkTextBoxes(gzipped(kFile) + "junk").has_value());
    EXPECT_FALSE(noteAppC::inkTextBoxes(R"({"format":"notolith-ink","items":[{"type":"textbox","markdown":"x"}]})").has_value());
    EXPECT_FALSE(noteAppC::inkTextBoxes(R"({"format":"notolith-ink","version":"1","items":[]})").has_value());
    EXPECT_FALSE(noteAppC::inkTextBoxes(R"({"format":"notolith-ink","version":0,"items":[]})").has_value());
}

TEST(InkTextBoxesTest, DeeplyNestedJSONDoesNotCrash)
{
    const QByteArray nested = QByteArray(100000, '[') + QByteArray(100000, ']');
    EXPECT_FALSE(noteAppC::inkTextBoxes(nested).has_value());
    noteAppC::inkTextBoxes(R"({"format":"notolith-ink","items":)" + nested + "}"); // either answer is fine: it must not crash
}
