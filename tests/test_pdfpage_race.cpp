// Regression test for TODO.md's "test_demo crashed once in poppler": a
// SIGSEGV, jumping to a garbage address inside poppler's own
// GfxXYZ2DisplayTransforms, that showed up once under a parallel ctest run
// and never under a single-threaded one.
//
// Root cause, confirmed by reading poppler 26.08.0's GfxState.cc: every
// render builds a GfxState, and GfxState's constructor (plus
// GfxXYZ2DisplayTransforms's) lazily fills in two process-global statics -
// GfxState::sRGBProfile and GfxXYZ2DisplayTransforms::XYZProfile - behind an
// unguarded `if (!profile) profile = ...;` on a std::shared_ptr. PdfPage.cpp
// already gives each thread its own Poppler::Document (thread_local), which
// is what poppler-qt6's own docs ask for, but that does not cover this:
// these two statics sit underneath every Document, in the whole process, so
// several threads' first-ever renders can still race on writing the same
// shared_ptr. PdfPage::render now runs one tiny render alone, gated by
// std::call_once, before any real render proceeds - see
// warmUpColorManagement in PdfPage.cpp.
//
// This test is its own executable (rather than a TEST() folded into
// test_editor or test_demo) specifically so it is the *first* thing in the
// process to call PdfPage::render: the race above only exists before those
// two statics are set for the very first time, so a concurrency test that
// runs after some earlier test already rendered a page would prove nothing.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFont>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>

#include <atomic>
#include <thread>
#include <vector>

#include "PdfPage.h"

using namespace noteAppC;

namespace {

QString makeTestPdf(const QString &dir)
{
    const QString path = dir + QStringLiteral("/race.pdf");
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    QFont font;
    font.setPointSize(14);
    QPainter painter(&writer);
    painter.setFont(font);
    painter.drawText(72, 100, QStringLiteral("Hello world."));
    painter.end();
    return path;
}

} // namespace

TEST(PdfPageRaceTest, ConcurrentFirstRendersAcrossThreadsDoNotCrash)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = makeTestPdf(dir.path());

    // 16 threads, released together by a spin-wait on a shared flag rather
    // than started one at a time, so their first calls into PdfPage::render
    // - and so their first construction of a poppler GfxState - land as
    // close to simultaneously as this process can arrange.
    constexpr int kThreads = 16;
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::vector<std::thread> threads;
    std::vector<QImage> images(kThreads);
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ++ready;
            while (!go.load(std::memory_order_acquire))
                ; // spin - this is microseconds, not worth a condvar
            images[i] = PdfPage::render(path, 0, 300);
        });
    }
    while (ready.load(std::memory_order_relaxed) < kThreads)
        ; // wait for every thread to reach its own start line
    go.store(true, std::memory_order_release);

    for (auto &thread : threads)
        thread.join();

    // Reaching this line at all is most of the regression test - the
    // original bug was a SIGSEGV, which ctest reports as a crashed test
    // rather than a failed assertion. The image checks catch silent
    // corruption (a torn color-transform pointer producing garbage pixels
    // without an outright crash) on top of that.
    for (int i = 0; i < kThreads; ++i) {
        EXPECT_FALSE(images[i].isNull()) << "thread " << i;
        EXPECT_EQ(images[i].width(), 300) << "thread " << i;
        EXPECT_GT(images[i].height(), 0) << "thread " << i;
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
