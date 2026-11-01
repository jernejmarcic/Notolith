// EPUB reader: archive access, EPUB2 (NCX) and EPUB3 (nav) parsing, spine
// order, the archive-path sandbox (../ escapes and remote URLs refused),
// script stripping, a highlight locator's round trip through the sidecar,
// the sidecar keeping the reader's own note text across re-extraction, and
// that a render scheduled before the view is destroyed does nothing after.
// Also: chapter rendering off the UI thread and the stale-render/UAF guards
// that come with it, a lazy table of contents, @font-face registration and
// CSS url() resource extraction, and paged mode - see docs/PERFORMANCE.md for the measured numbers.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QFontDatabase>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <archive.h>
#include <archive_entry.h>

#include "WaitFor.h"

#include "EpubArchive.h"
#include "EpubCover.h"
#include "MarkdownEditor.h"
#include "EpubDocument.h"
#include "EpubHighlights.h"
#include "EpubView.h"
#include "HtmlRenderer.h"
#include "Vault.h"
#include "Perf.h"

using namespace noteAppC;

namespace {

/// Writes a zip - a minimal EPUB is nothing more - from an ordered list of
/// {entryName, bytes}. Real EPUB toolchains all do the same thing; libarchive
/// is already this module's own dependency, so the tests build their fixture
/// books the same way EpubArchive reads them, rather than pulling in a
/// second zip library just for test setup.
void writeZip(const QString &path, const QList<std::pair<QString, QByteArray>> &entries)
{
    archive *archiveHandle = archive_write_new();
    archive_write_set_format_zip(archiveHandle);
    ASSERT_EQ(archive_write_open_filename(archiveHandle, path.toUtf8().constData()), ARCHIVE_OK);

    for (const auto &[name, bytes] : entries) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, bytes.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        ASSERT_EQ(archive_write_header(archiveHandle, entry), ARCHIVE_OK);
        archive_write_data(archiveHandle, bytes.constData(), size_t(bytes.size()));
        archive_entry_free(entry);
    }
    archive_write_close(archiveHandle);
    archive_write_free(archiveHandle);
}

const char *kContainerXML =
    "<?xml version='1.0'?>"
    "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'>"
    "<rootfiles><rootfile full-path='OEBPS/content.opf' "
    "media-type='application/oebps-package+xml'/></rootfiles></container>";

/// A minimal-but-real EPUB 3: nav.xhtml for the table of contents,
/// properties="nav" on its manifest item, two chapters, a stylesheet, and an
/// image - everything resource resolution and spine order need to exercise.
QString makeEpub3(const QString &dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("book3.epub"));
    const QByteArray opf = QByteArrayLiteral(R"(<?xml version='1.0'?>
<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>
  <metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>
    <dc:title>Test Book Three</dc:title>
    <dc:creator>A. Uthor</dc:creator>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>
    <item id='c1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>
    <item id='c2' href='text/ch2.xhtml' media-type='application/xhtml+xml'/>
    <item id='css' href='styles/book.css' media-type='text/css'/>
    <item id='img' href='images/pic.png' media-type='image/png'/>
  </manifest>
  <spine>
    <itemref idref='c1'/>
    <itemref idref='c2'/>
  </spine>
</package>)");
    const QByteArray nav = QByteArrayLiteral(R"(<?xml version='1.0'?>
<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'>
<body><nav epub:type='toc'><ol>
  <li><a href='text/ch1.xhtml'>Chapter One</a></li>
  <li><a href='text/ch2.xhtml'>Chapter Two</a></li>
</ol></nav></body></html>)");
    const QByteArray ch1 = QByteArrayLiteral(R"(<?xml version='1.0'?>
<html xmlns='http://www.w3.org/1999/xhtml'><head>
  <link rel='stylesheet' href='../styles/book.css'/>
  <script>window.evil = true;</script>
</head>
<body>
  <p onclick='window.evil2 = true'>This is the first chapter of the book.</p>
  <img src='../images/pic.png'/>
  <p>It has more than one paragraph so offsets are meaningful.</p>
</body></html>)");
    const QByteArray ch2 = QByteArrayLiteral(
        "<?xml version='1.0'?><html xmlns='http://www.w3.org/1999/xhtml'><body>"
        "<p>Second chapter text, for spine order.</p>"
        // A resource reference that tries to escape the archive and one that
        // is remote - both must be refused, never read or fetched.
        "<img src='../../../../etc/passwd'/>"
        "<img src='https://example.com/tracker.png'/>"
        "</body></html>");
    const QByteArray css = QByteArrayLiteral("body { color: rgb(10, 20, 30); }");
    // The smallest possible valid PNG (a 1x1 transparent pixel) - only its
    // presence/bytes matter to this test, not its pixels.
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");

    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"), QByteArray(kContainerXML)},
                    {QStringLiteral("OEBPS/content.opf"), opf},
                    {QStringLiteral("OEBPS/nav.xhtml"), nav},
                    {QStringLiteral("OEBPS/text/ch1.xhtml"), ch1},
                    {QStringLiteral("OEBPS/text/ch2.xhtml"), ch2},
                    {QStringLiteral("OEBPS/styles/book.css"), css},
                    {QStringLiteral("OEBPS/images/pic.png"), png}});
    return path;
}

/// Two chapters of many short paragraphs, each taller than a 500x400 view
/// once rendered. makeEpub3's chapters fit on one page together, so a test
/// that needs to scroll there only works while the heights are still the
/// 600 px guess - i.e. until the background renders land.
QString makeTallEpub3(const QString &dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("tall.epub"));
    const QByteArray opf = QByteArrayLiteral(R"(<?xml version='1.0'?>
<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>
  <metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:title>Tall</dc:title></metadata>
  <manifest>
    <item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>
    <item id='c1' href='ch1.xhtml' media-type='application/xhtml+xml'/>
    <item id='c2' href='ch2.xhtml' media-type='application/xhtml+xml'/>
  </manifest>
  <spine><itemref idref='c1'/><itemref idref='c2'/></spine>
</package>)");
    const QByteArray nav = QByteArrayLiteral(R"(<?xml version='1.0'?>
<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'>
<body><nav epub:type='toc'><ol>
  <li><a href='ch1.xhtml'>One</a></li><li><a href='ch2.xhtml'>Two</a></li>
</ol></nav></body></html>)");
    QByteArray chapter = "<?xml version='1.0'?><html xmlns='http://www.w3.org/1999/xhtml'><body>";
    for (int i = 0; i < 40; ++i)
        chapter += "<p>A paragraph of filler text.</p>";
    chapter += "</body></html>";

    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"), QByteArray(kContainerXML)},
                    {QStringLiteral("OEBPS/content.opf"), opf},
                    {QStringLiteral("OEBPS/nav.xhtml"), nav},
                    {QStringLiteral("OEBPS/ch1.xhtml"), chapter},
                    {QStringLiteral("OEBPS/ch2.xhtml"), chapter}});
    return path;
}

/// Shows the view and waits until every chapter has rendered, so its height
/// is exact rather than the 600 px guess - otherwise the scroll range a test
/// sees depends on how far the background renders got under load.
void showAndSettle(EpubView &view)
{
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    view.requestChapterForTest(0);
    view.requestChapterForTest(1);
    WAIT_TRUE(view.chapterCachedForTest(0) && view.chapterCachedForTest(1) && view.pendingRenderCountForTest() == 0,
              5000);
}

/// A minimal EPUB 2: toc.ncx instead of nav.xhtml, spine@toc pointing at it.
QString makeEpub2(const QString &dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("book2.epub"));
    const QByteArray opf = QByteArrayLiteral(R"(<?xml version='1.0'?>
<package xmlns='http://www.idpf.org/2007/opf' version='2.0' unique-identifier='id'>
  <metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>
    <dc:title>Test Book Two</dc:title>
    <dc:creator>B. Writer</dc:creator>
  </metadata>
  <manifest>
    <item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>
    <item id='c1' href='ch1.xhtml' media-type='application/xhtml+xml'/>
    <item id='c2' href='ch2.xhtml' media-type='application/xhtml+xml'/>
  </manifest>
  <spine toc='ncx'>
    <itemref idref='c1'/>
    <itemref idref='c2'/>
  </spine>
</package>)");
    const QByteArray ncx = QByteArrayLiteral(R"(<?xml version='1.0'?>
<ncx xmlns='http://www.daisy.org/z3986/2005/ncx/'><navMap>
  <navPoint id='n1'><navLabel><text>First</text></navLabel><content src='ch1.xhtml'/></navPoint>
  <navPoint id='n2'><navLabel><text>Second</text></navLabel><content src='ch2.xhtml'/></navPoint>
</navMap></ncx>)");
    const QByteArray ch1 = QByteArrayLiteral(
        "<html><body><p>EPUB2 chapter one.</p></body></html>");
    const QByteArray ch2 = QByteArrayLiteral(
        "<html><body><p>EPUB2 chapter two.</p></body></html>");

    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"),
                     QByteArrayLiteral("<?xml version='1.0'?><container "
                                       "xmlns='urn:oasis:names:tc:opendocument:xmlns:container' "
                                       "version='1.0'><rootfiles><rootfile full-path='content.opf' "
                                       "media-type='application/oebps-package+xml'/></rootfiles></container>")},
                    {QStringLiteral("content.opf"), opf},
                    {QStringLiteral("toc.ncx"), ncx},
                    {QStringLiteral("ch1.xhtml"), ch1},
                    {QStringLiteral("ch2.xhtml"), ch2}});
    return path;
}

/// A book with a huge, two-level table of contents - `volumeCount` top-level
/// entries, each with `chaptersPerVolume` children - and exactly one real
/// spine chapter every entry points at (the lazy-TOC fixture only needs the tree
/// shape, not distinct targets for each of possibly thousands of rows).
QString makeEpubWithHugeToc(const QString &dir, int volumeCount, int chaptersPerVolume)
{
    const QString path = QDir(dir).filePath(QStringLiteral("hugetoc.epub"));
    const QByteArray opf = QByteArrayLiteral(R"(<?xml version='1.0'?>
<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>
  <metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>
    <dc:title>Huge Toc Book</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>
    <item id='c1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>
  </manifest>
  <spine><itemref idref='c1'/></spine>
</package>)");
    const QByteArray ch1 = QByteArrayLiteral("<html><body><p>Only chapter.</p></body></html>");

    QString navBody;
    for (int v = 0; v < volumeCount; ++v) {
        navBody += QStringLiteral("<li><a href='text/ch1.xhtml'>Volume %1</a><ol>").arg(v);
        for (int c = 0; c < chaptersPerVolume; ++c)
            navBody += QStringLiteral("<li><a href='text/ch1.xhtml'>Volume %1 Chapter %2</a></li>").arg(v).arg(c);
        navBody += QStringLiteral("</ol></li>");
    }
    const QByteArray nav = (QStringLiteral("<?xml version='1.0'?>"
                                           "<html xmlns='http://www.w3.org/1999/xhtml' "
                                           "xmlns:epub='http://www.idpf.org/2007/ops'>"
                                           "<body><nav epub:type='toc'><ol>")
                            + navBody + QStringLiteral("</ol></nav></body></html>"))
                               .toUtf8();

    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"), QByteArray(kContainerXML)},
                    {QStringLiteral("OEBPS/content.opf"), opf},
                    {QStringLiteral("OEBPS/nav.xhtml"), nav},
                    {QStringLiteral("OEBPS/text/ch1.xhtml"), ch1}});
    return path;
}

/// A book whose stylesheet declares two `@font-face` rules (one local, one
/// remote - never fetched) and a `background-image` - the CSS-resource fixture.
/// `fontBytes` is embedded verbatim as the local font's source; the caller
/// decides whether that is a real font (to check registration succeeds) or
/// garbage (to check a failed load still degrades quietly).
QString makeEpubWithFontAndBackground(const QString &dir, const QByteArray &fontBytes)
{
    const QString path = QDir(dir).filePath(QStringLiteral("fontbg.epub"));
    const QByteArray opf = QByteArrayLiteral(R"(<?xml version='1.0'?>
<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>
  <metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>
    <dc:title>Font And Background Book</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>
    <item id='c1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>
    <item id='css' href='styles/book.css' media-type='text/css'/>
    <item id='font' href='fonts/test.ttf' media-type='font/ttf'/>
    <item id='bg' href='images/bg.png' media-type='image/png'/>
  </manifest>
  <spine><itemref idref='c1'/></spine>
</package>)");
    const QByteArray nav = QByteArrayLiteral(R"(<?xml version='1.0'?>
<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'>
<body><nav epub:type='toc'><ol><li><a href='text/ch1.xhtml'>One</a></li></ol></nav></body></html>)");
    const QByteArray ch1 = QByteArrayLiteral(R"(<?xml version='1.0'?>
<html xmlns='http://www.w3.org/1999/xhtml'><head>
  <link rel='stylesheet' href='../styles/book.css'/>
</head><body><p>Styled chapter text.</p></body></html>)");
    const QByteArray css = QByteArrayLiteral(
        "@font-face { font-family: 'EpubTestFont'; src: url('../fonts/test.ttf'); }\n"
        "@font-face { font-family: 'RemoteFont'; src: url('https://example.com/remote.ttf'); }\n"
        "body { font-family: 'EpubTestFont'; background-image: url('../images/bg.png'); }\n");
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");

    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"), QByteArray(kContainerXML)},
                    {QStringLiteral("OEBPS/content.opf"), opf},
                    {QStringLiteral("OEBPS/nav.xhtml"), nav},
                    {QStringLiteral("OEBPS/text/ch1.xhtml"), ch1},
                    {QStringLiteral("OEBPS/styles/book.css"), css},
                    {QStringLiteral("OEBPS/fonts/test.ttf"), fontBytes},
                    {QStringLiteral("OEBPS/images/bg.png"), png}});
    return path;
}

} // namespace

// --- EpubArchive -----------------------------------------------------------

TEST(EpubArchiveTest, IndexesAndReadsEntries)
{
    QTemporaryDir dir;
    const QString path = makeEpub3(dir.path());

    EpubArchive archive;
    QString error;
    ASSERT_TRUE(archive.open(path, &error)) << error.toStdString();
    EXPECT_TRUE(archive.contains(QStringLiteral("OEBPS/content.opf")));
    EXPECT_TRUE(archive.contains(QStringLiteral("OEBPS/text/ch1.xhtml")));
    EXPECT_FALSE(archive.contains(QStringLiteral("nothing/here")));

    const QByteArray css = archive.read(QStringLiteral("OEBPS/styles/book.css"));
    EXPECT_EQ(css, QByteArrayLiteral("body { color: rgb(10, 20, 30); }"));
}

TEST(EpubArchiveTest, RefusesANonZipFile)
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("notabook.epub"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not a zip file at all");
    file.close();

    EpubArchive archive;
    QString error;
    EXPECT_FALSE(archive.open(path, &error));
    EXPECT_FALSE(error.isEmpty());
}

// --- EpubDocument: parsing, spine order, resource sandboxing ---------------

TEST(EpubDocumentTest, ParsesEpub3MetadataSpineAndToc)
{
    QTemporaryDir dir;
    EpubDocument document;
    QString error;
    ASSERT_TRUE(document.open(makeEpub3(dir.path()), &error)) << error.toStdString();

    EXPECT_EQ(document.title(), QStringLiteral("Test Book Three"));
    EXPECT_EQ(document.author(), QStringLiteral("A. Uthor"));

    ASSERT_EQ(document.spine().size(), 2);
    EXPECT_EQ(document.spine().at(0).archivePath, QStringLiteral("OEBPS/text/ch1.xhtml"));
    EXPECT_EQ(document.spine().at(1).archivePath, QStringLiteral("OEBPS/text/ch2.xhtml"));
    EXPECT_EQ(document.spineIndexOf(QStringLiteral("OEBPS/text/ch2.xhtml")), 1);

    ASSERT_EQ(document.tableOfContents().size(), 2);
    EXPECT_EQ(document.tableOfContents().at(0).title, QStringLiteral("Chapter One"));
    EXPECT_EQ(document.tableOfContents().at(0).spineIndex, 0);
    EXPECT_EQ(document.tableOfContents().at(1).spineIndex, 1);
}

TEST(EpubDocumentTest, ParsesEpub2NCXTableOfContentsInSpineOrder)
{
    QTemporaryDir dir;
    EpubDocument document;
    QString error;
    ASSERT_TRUE(document.open(makeEpub2(dir.path()), &error)) << error.toStdString();

    ASSERT_EQ(document.spine().size(), 2);
    EXPECT_EQ(document.spine().at(0).archivePath, QStringLiteral("ch1.xhtml"));
    EXPECT_EQ(document.spine().at(1).archivePath, QStringLiteral("ch2.xhtml"));

    ASSERT_EQ(document.tableOfContents().size(), 2);
    EXPECT_EQ(document.tableOfContents().at(0).title, QStringLiteral("First"));
    EXPECT_EQ(document.tableOfContents().at(1).title, QStringLiteral("Second"));
}

TEST(EpubDocumentTest, ResolvesARelativeHrefAgainstItsOwnEntry)
{
    const auto resolved = EpubDocument::resolveArchivePath(QStringLiteral("OEBPS/text/ch1.xhtml"),
                                                            QStringLiteral("../images/pic.png"));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, QStringLiteral("OEBPS/images/pic.png"));
}

TEST(EpubDocumentTest, RefusesAPathThatEscapesTheArchiveRoot)
{
    // Guard checked by breaking it: temporarily resolving with cleanPath()
    // alone (no root-walk) let "../../../../etc/passwd" through as
    // "etc/passwd", and this test failed (a non-nullopt result) until the
    // manual walk in resolveArchivePath was restored - see EpubDocument.cpp.
    EXPECT_FALSE(EpubDocument::resolveArchivePath(QStringLiteral("OEBPS/text/ch2.xhtml"),
                                                  QStringLiteral("../../../../etc/passwd"))
                    .has_value());
    EXPECT_FALSE(EpubDocument::resolveArchivePath(QStringLiteral("a/b.xhtml"), QStringLiteral("../../c")).has_value());
}

TEST(EpubDocumentTest, RefusesRemoteAndDataURIs)
{
    EXPECT_FALSE(EpubDocument::resolveArchivePath(QStringLiteral("a/b.xhtml"),
                                                  QStringLiteral("https://example.com/tracker.png"))
                    .has_value());
    EXPECT_FALSE(
        EpubDocument::resolveArchivePath(QStringLiteral("a/b.xhtml"), QStringLiteral("data:image/png;base64,AA=="))
            .has_value());
    EXPECT_FALSE(EpubDocument::resolveArchivePath(QStringLiteral("a/b.xhtml"), QStringLiteral("/etc/passwd"))
                    .has_value());
}

// --- scripts never reach litehtml ------------------------------------------

TEST(EpubDocumentTest, ScriptsAndEventHandlersAreStripped)
{
    QTemporaryDir dir;
    EpubDocument document;
    ASSERT_TRUE(document.open(makeEpub3(dir.path())));
    const QByteArray raw = document.readEntry(QStringLiteral("OEBPS/text/ch1.xhtml"));
    ASSERT_FALSE(raw.isEmpty());
    ASSERT_TRUE(QString::fromUtf8(raw).contains(QStringLiteral("<script>")));

    const QString sanitized = HtmlRenderer::sanitizeHtml(QString::fromUtf8(raw));
    EXPECT_FALSE(sanitized.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
    EXPECT_FALSE(sanitized.contains(QStringLiteral("window.evil")));
    EXPECT_FALSE(sanitized.contains(QStringLiteral("onclick")));
    EXPECT_TRUE(sanitized.contains(QStringLiteral("This is the first chapter")));
}

// --- highlights: locator format, sidecar round trip, note preservation ----

TEST(EpubLocatorTest, RoundTripsThroughItsStringForm)
{
    EpubLocator locator{3, 128, 172};
    const QString text = locator.toString();
    EXPECT_EQ(text, QStringLiteral("epub:3:128-172"));
    const auto parsed = EpubLocator::parse(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, locator);
}

TEST(EpubHighlightsTest, LocatorAndQuoteRoundTripThroughTheSidecar)
{
    QTemporaryDir dir;
    const QString sidecar = QDir(dir.path()).filePath(QStringLiteral("Book highlights.md"));

    EpubHighlight highlight;
    highlight.locator = {0, 10, 40};
    highlight.color = QStringLiteral("yellow");
    highlight.quote = QStringLiteral("Calibre is available for Windows, macOS and Linux.");
    highlight.note = QStringLiteral("Good to know.");

    QString error;
    ASSERT_TRUE(EpubHighlights::writeSidecar(sidecar, QStringLiteral("Book.epub"), QStringLiteral("Book"),
                                             {highlight}, nullptr, &error))
        << error.toStdString();

    Note note;
    ASSERT_TRUE(Vault::read(sidecar, note));
    const QList<EpubHighlight> reparsed = EpubHighlights::parse(note.text);
    ASSERT_EQ(reparsed.size(), 1);
    EXPECT_EQ(reparsed.at(0).locator, highlight.locator);
    EXPECT_EQ(reparsed.at(0).color, highlight.color);
    EXPECT_EQ(reparsed.at(0).quote, highlight.quote);
    EXPECT_EQ(reparsed.at(0).note, highlight.note);
}

TEST(EpubHighlightsTest, ReExtractionKeepsTheReadersOwnNoteText)
{
    QTemporaryDir dir;
    const QString sidecar = QDir(dir.path()).filePath(QStringLiteral("Book highlights.md"));

    EpubHighlight highlight;
    highlight.locator = {0, 10, 40};
    highlight.color = QStringLiteral("yellow");
    highlight.quote = QStringLiteral("A quote from the book.");
    highlight.note = QStringLiteral("Original note.");
    ASSERT_TRUE(EpubHighlights::writeSidecar(sidecar, QStringLiteral("Book.epub"), QStringLiteral("Book"),
                                             {highlight}, nullptr));

    // The reader edits the note by hand, in the file - exactly what
    // "extract highlights" must not clobber.
    Note note;
    ASSERT_TRUE(Vault::read(sidecar, note));
    QString edited = note.text;
    edited.replace(QStringLiteral("Original note."), QStringLiteral("The reader's own rewritten note."));
    note.text = edited;
    ASSERT_TRUE(Vault::write(sidecar, note));

    // Re-extraction with the *same* live highlight (its own .note field
    // still says "Original note." - only what is on disk changed) must keep
    // what the reader wrote, not overwrite it back to the app's own copy.
    ASSERT_TRUE(EpubHighlights::writeSidecar(sidecar, QStringLiteral("Book.epub"), QStringLiteral("Book"),
                                             {highlight}, nullptr));
    Note final;
    ASSERT_TRUE(Vault::read(sidecar, final));
    const QList<EpubHighlight> reparsed = EpubHighlights::parse(final.text);
    ASSERT_EQ(reparsed.size(), 1);
    EXPECT_EQ(reparsed.at(0).note, QStringLiteral("The reader's own rewritten note."));

    // Text outside the generated section survives verbatim, too.
    EXPECT_TRUE(final.text.contains(QStringLiteral("Links back into [[Book.epub]]")));
}

// --- a render scheduled before destruction delivers nowhere ---------------

TEST(EpubViewTest, OpensABookAndScrollingSchedulesNoUseAfterFree)
{
    QTemporaryDir dir;
    const QString path = makeEpub3(dir.path());

    auto *view = new EpubView;
    view->resize(500, 400);
    QString error;
    ASSERT_TRUE(view->load(path, &error)) << error.toStdString();
    view->show();
    // A real paint, so paintContent() actually runs and schedules the
    // look-ahead prefetch (QPointer-guarded QTimer::singleShot - see
    // EpubRender.cpp's paintContent()) whose whole job is doing nothing once
    // `view` is gone.
    QTest::qWait(30);

    delete view;
    // If the guard were broken (a raw `this` captured instead of the
    // QPointer, so the lambda still calls renderedChapter() on the freed
    // object) this either crashes or trips ASAN; under a plain build it is
    // silent either way, which is why an ASAN pass is needed on
    // any lifetime change - this test's only job is to give that pass
    // something to run.
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();
    SUCCEED();
}

TEST(EpubViewTest, NavigatesToALocatorFragment)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));
    view.show();
    QTest::qWait(10);

    EXPECT_TRUE(view.navigateToFragment(QStringLiteral("epub:1:0-5")));
    EXPECT_TRUE(view.navigateToFragment(QStringLiteral("1")));
    EXPECT_FALSE(view.navigateToFragment(QStringLiteral("epub:99:0-5")));
}

// --- chapter rendering moves off the UI thread ------------------------------

TEST(EpubViewTest, ChapterRenderRunsOffThreadAndArrivesAsynchronously)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));

    ASSERT_FALSE(view.chapterCachedForTest(0));
    view.requestChapterForTest(0);
    // Not cached yet: the litehtml layout/raster itself never runs inline on
    // this (the test's, i.e. the UI) thread any more - see
    // EpubView::requestChapterRender(). A synchronous implementation would
    // already have this chapter cached by the time the call above returns.
    EXPECT_FALSE(view.chapterCachedForTest(0));
    EXPECT_GE(view.pendingRenderCountForTest(), 1);

    WAIT_TRUE(view.chapterCachedForTest(0), 2000);
    EXPECT_EQ(view.pendingRenderCountForTest(), 0);
}

TEST(EpubViewTest, ARenderAlreadyInFlightIsNotDuplicated)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));

    view.requestChapterForTest(0);
    const int pendingAfterFirst = view.pendingRenderCountForTest();
    ASSERT_GE(pendingAfterFirst, 1);
    view.requestChapterForTest(0); // same chapter, still in flight - see m_pendingSpine
    EXPECT_EQ(view.pendingRenderCountForTest(), pendingAfterFirst);

    WAIT_TRUE(view.chapterCachedForTest(0), 2000);
}

TEST(EpubViewTest, AStaleRenderFromBeforeAResizeIsDiscardedNotCached)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));

    view.requestChapterForTest(0);
    view.resize(300, 250); // invalidateRenders() bumps m_renderEpoch mid-flight
    view.requestChapterForTest(0); // the fresh request, at the new width

    // Both the stale and the fresh render eventually land; only one chapter
    // ever ends up cached (deliverChapterRender drops the stale one by
    // epoch, exactly like a resize the old synchronous code never had to
    // worry about because nothing was ever in flight to begin with).
    WAIT_TRUE(view.chapterCachedForTest(0), 2000);
    WAIT_TRUE(view.pendingRenderCountForTest() == 0, 2000);
    EXPECT_TRUE(view.chapterCachedForTest(0));
}

TEST(EpubViewTest, DeletingTheViewWhileAWorkerRenderIsInFlightIsSafe)
{
    QTemporaryDir dir;
    const QString path = makeEpub3(dir.path());

    auto *view = new EpubView;
    view->resize(500, 400);
    ASSERT_TRUE(view->load(path));
    view->requestChapterForTest(0);
    view->requestChapterForTest(1);
    // Deleted with a worker render still on its way to m_lifeline->post() -
    // the exact race Lifeline::end(), called first in ~EpubView, exists for
    // (see EpubView.cpp and Lifeline.h). If that ordering were broken (end()
    // moved later, or the worker captured a raw `this` instead of going
    // through the lifeline) this either crashes or trips ASAN.
    delete view;
    QCoreApplication::processEvents();
    QTest::qWait(80); // long enough for a real machine's worker to finish and try to post
    QCoreApplication::processEvents();
    SUCCEED();
}

// --- lazy table of contents -------------------------------------------------

TEST(EpubViewTest, TocIsNotBuiltAtOpenEvenForAHugeBook)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpubWithHugeToc(dir.path(), 20, 100))); // 2000 entries

    EXPECT_FALSE(view.tocBuiltForTest());
    EXPECT_EQ(view.tocTopLevelCountForTest(), 0);
}

TEST(EpubViewTest, TocBuildsOnlyTheTopLevelOnFirstShow)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpubWithHugeToc(dir.path(), 20, 100)));

    view.setTocVisibleForTest(true);
    EXPECT_TRUE(view.tocBuiltForTest());
    ASSERT_EQ(view.tocTopLevelCountForTest(), 20);
    // Every volume carries its single placeholder row, not its 100 real
    // chapters, until it is actually expanded - see populateTocTree().
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(view.tocChildCountForTest(i), 1) << "volume " << i;
}

TEST(EpubViewTest, TocExpandsOneNodeAtATimeAndOnlyOnce)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpubWithHugeToc(dir.path(), 20, 100)));
    view.setTocVisibleForTest(true);

    view.expandTocForTest(0);
    EXPECT_EQ(view.tocChildCountForTest(0), 100);
    EXPECT_EQ(view.tocChildCountForTest(1), 1); // every other volume untouched

    // The populated guard (onTocExpanded): expanding the same node again
    // must not duplicate its rows. Broken (commenting out the guard) once
    // showed 200 children here instead of 100, confirming the test catches
    // it; restored before committing.
    view.expandTocForTest(0);
    EXPECT_EQ(view.tocChildCountForTest(0), 100);
}

// --- CSS url() resources - fonts and background images ----------------------

TEST(EpubViewTest, FontFaceIsRegisteredSynchronouslyBeforeTheWorkerIsDispatched)
{
    QFile fontFile(QStringLiteral("/usr/share/fonts/rnote-fonts/GrapeNuts-Regular.ttf"));
    if (!fontFile.open(QIODevice::ReadOnly))
        GTEST_SKIP() << "system font fixture not present on this machine";
    const QByteArray fontBytes = fontFile.readAll();
    ASSERT_FALSE(fontBytes.isEmpty());

    // Learn the font's real family name independently of EpubView, so the
    // assertion below never hardcodes a name that lives inside a system
    // font file this test does not own.
    const int probeID = QFontDatabase::addApplicationFontFromData(fontBytes);
    ASSERT_NE(probeID, -1);
    const QString family = QFontDatabase::applicationFontFamilies(probeID).value(0);
    ASSERT_FALSE(family.isEmpty());
    QFontDatabase::removeApplicationFont(probeID);

    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpubWithFontAndBackground(dir.path(), fontBytes)));

    EXPECT_EQ(view.registeredFontCountForTest(), 0);
    view.requestChapterForTest(0);
    // Registered right here, before this call even returns - not after the
    // (asynchronous) render lands. That ordering is the point:
    // QFontDatabase::addApplicationFontFromData is not safe to call from the
    // litehtml worker thread (see EpubView::registerChapterFonts's comment
    // and PageBody.h's identical rule for MathRenderer/MicroTeX), so it has
    // to happen here, synchronously, on the thread that called
    // requestChapterRender() - never inside the QtConcurrent::run lambda.
    EXPECT_EQ(view.registeredFontCountForTest(), 1);
    // An application font also shows up in the regular family list (Qt's
    // own guarantee), which is what lets create_font()'s plain
    // QFont(family) (HtmlRenderer.cpp) find it with no EpubView-specific
    // lookup of its own.
    EXPECT_TRUE(QFontDatabase::families().contains(family));
}

TEST(EpubViewTest, RemoteFontFaceIsNeverFetched)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(
        view.load(makeEpubWithFontAndBackground(dir.path(), QByteArrayLiteral("not a real font file"))));

    view.requestChapterForTest(0);
    // The fixture always declares two @font-face rules (see
    // makeEpubWithFontAndBackground) - one local, one remote. Only the
    // local one is ever attempted; a count of 2 here would mean the remote
    // one was fetched.
    EXPECT_EQ(view.registeredFontCountForTest(), 1);
}

TEST(EpubViewTest, BackgroundImageUrlInCssIsExtractedFromTheArchive)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(
        view.load(makeEpubWithFontAndBackground(dir.path(), QByteArrayLiteral("not a real font file"))));

    view.requestChapterForTest(0);
    WAIT_TRUE(view.chapterCachedForTest(0), 2000);

    const QString expected = view.cacheRootForTest() + QStringLiteral("/res/OEBPS/images/bg.png");
    EXPECT_TRUE(QFileInfo::exists(expected));
}

// --- paged mode -------------------------------------------------------------

TEST(EpubViewTest, PagedModeTurnsByExactlyOnePageHeight)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 60); // tiny viewport: even this fixture's short chapters span several pages
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));
    view.show();
    QTest::qWait(10);

    view.setPagedModeForTest(true);
    ASSERT_TRUE(view.pagedModeForTest());
    EXPECT_EQ(view.scrollValueForTest(), 0);

    const qint64 ph = view.pageHeightForTest();
    const qint64 step = ph * (view.pagedColumnsForTest() == 2 ? 2 : 1);
    view.turnPageForTest(1);
    EXPECT_EQ(view.scrollValueForTest(), step);

    view.turnPageForTest(-1);
    EXPECT_EQ(view.scrollValueForTest(), 0);
}

TEST(EpubViewTest, PagedModeNeverScrollsPastTheDocumentBounds)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 60);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));
    view.show();
    QTest::qWait(10);
    view.setPagedModeForTest(true);

    view.turnPageForTest(-1); // already at the start
    EXPECT_EQ(view.scrollValueForTest(), 0);

    for (int i = 0; i < 200; ++i)
        view.turnPageForTest(1); // walk well past the end
    EXPECT_EQ(view.scrollValueForTest(), view.scrollMaximumForTest());
}

TEST(EpubViewTest, PagedModeUsesTwoColumnsOnAWideViewAndOneOnANarrowOne)
{
    QTemporaryDir dir;
    EpubView view;
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));

    view.resize(1200, 600);
    view.show();
    QTest::qWait(10);
    view.setPagedModeForTest(true);
    EXPECT_EQ(view.pagedColumnsForTest(), 2);

    view.resize(500, 600);
    QTest::qWait(10);
    EXPECT_EQ(view.pagedColumnsForTest(), 1);
}

TEST(EpubViewTest, MarginClicksTurnPagesOnlyInPagedMode)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeTallEpub3(dir.path())));
    ASSERT_NO_FATAL_FAILURE(showAndSettle(view));

    // Continuous mode: a margin click is just a click, never a page turn.
    EXPECT_FALSE(view.marginClickForTest(2, 50));

    view.setPagedModeForTest(true);
    EXPECT_TRUE(view.marginClickForTest(2, 50)); // left margin -> previous (clamped at 0)
    EXPECT_EQ(view.scrollValueForTest(), 0);
    EXPECT_TRUE(view.marginClickForTest(498, 50)); // right margin -> next
    EXPECT_GT(view.scrollValueForTest(), 0);
    EXPECT_FALSE(view.marginClickForTest(250, 50)); // the middle of the page: not a margin
}

TEST(EpubViewTest, SwitchingToPagedModePreservesTheReadingPositionApproximately)
{
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeTallEpub3(dir.path())));
    ASSERT_NO_FATAL_FAILURE(showAndSettle(view));

    ASSERT_TRUE(view.navigateToFragment(QStringLiteral("1"))); // spine index 1
    const qint64 before = view.scrollValueForTest();
    ASSERT_GT(before, 0);

    view.setPagedModeForTest(true);
    // Snapped to the nearest page boundary at or before where continuous
    // mode left off, not reset to the start of the book - paged mode does
    // not force a chapter to start on a fresh page (see EpubView's class
    // comment / the report), so "preserves position" here means "within one
    // page height of it", not bit-for-bit identical.
    const qint64 afterPaged = view.scrollValueForTest();
    EXPECT_GE(afterPaged, 0);
    EXPECT_LE(before - afterPaged, view.pageHeightForTest());

    view.setPagedModeForTest(false);
    EXPECT_EQ(view.scrollValueForTest(), afterPaged); // continuous mode does not itself re-snap
}

TEST(EpubViewTest, HighlightsPaintInPagedModeWithoutCrashing)
{
    QTemporaryDir dir;
    const QString bookPath = makeEpub3(dir.path());
    EpubHighlight highlight;
    highlight.locator = {0, 0, 10};
    highlight.color = QStringLiteral("yellow");
    highlight.quote = QStringLiteral("This is the");
    ASSERT_TRUE(EpubHighlights::writeSidecar(EpubHighlights::sidecarPath(bookPath),
                                             QFileInfo(bookPath).fileName(), QStringLiteral("Test Book Three"),
                                             {highlight}, nullptr));

    EpubView view;
    view.resize(500, 60);
    ASSERT_TRUE(view.load(bookPath));
    ASSERT_EQ(view.highlights().size(), 1);

    view.setPagedModeForTest(true);
    view.show();
    QTest::qWait(60); // a real paint: paintPaged -> paintPage -> paintHighlightsAndSelection
    SUCCEED();
}

// --- a real book, if this machine happens to have one -----------------------

TEST(EpubViewTest, OpensARealCalibreSampleBookIfPresentOnThisMachine)
{
    // Not a downloaded fixture - real .epub files already live under
    // /usr/share on some machines (calibre's quick-start library). Skips
    // cleanly where that is not installed rather than failing the suite
    // over a file this test does not control.
    const QString path = QStringLiteral("/usr/share/calibre/quick_start/eng.epub");
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "calibre quick-start sample not installed on this machine";

    EpubView view;
    view.resize(700, 500);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    view.show();
    QTest::qWait(200); // real, non-trivial litehtml layout - give the worker time to land
    view.setPagedModeForTest(true);
    QTest::qWait(80);
    view.setTocVisibleForTest(true);
    SUCCEED(); // this test's only job is to not crash/hang on real-world markup
}

// --- measured numbers for the report ----------------------------------

// These three print through fprintf(stderr, ...) - the project's own
// throwaway-probe convention (see docs/PERFORMANCE.md's "Measuring"
// section) - alongside a loose sanity bound each (ratios/orders of
// magnitude, not tight budgets - PERFORMANCE.md rule 3), so they stay
// useful as regression smoke without becoming flaky wall-clock guards.

TEST(EpubViewTest, PerfChapterRenderDispatchStaysOffTheUiThread)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpub3(dir.path())));

    QElapsedTimer timer;
    timer.start();
    view.requestChapterForTest(0);
    // Best of three, the repeats on a fresh view each (a chapter is only ever
    // dispatched once): load only ever adds time.
    const qint64 dispatchMs = perf::bestOf([&, first = true]() mutable {
        if (std::exchange(first, false))
            return qint64(timer.elapsed());
        QTemporaryDir otherDir;
        EpubView other;
        other.resize(500, 400);
        if (!other.load(makeEpub3(otherDir.path())))
            return qint64(1) << 40;
        QElapsedTimer again;
        again.start();
        other.requestChapterForTest(0);
        return qint64(again.elapsed());
    }, 3, 5);
    // The UI thread's own share of the ~11 ms/chapter litehtml cost is just
    // building the request and handing it to QtConcurrent::run, not the
    // layout/raster itself.
    EXPECT_LT(dispatchMs, 5);

    timer.restart();
    WAIT_TRUE(view.chapterCachedForTest(0), 2000);
    const qint64 totalMs = timer.elapsed();
    fprintf(stderr,
            "[epub perf] chapter render: dispatch (blocks the UI thread) %lld ms, "
            "total incl. worker (does not block it) %lld ms - both were once the same "
            "~11 ms, all on the UI thread\n",
            (long long)dispatchMs, (long long)totalMs);
}

TEST(EpubViewTest, PerfPageTurnReturnsImmediately)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three, each on a fresh view: load only ever adds time.
    const qint64 turnMs = perf::bestOf([] {
        QTemporaryDir dir;
        EpubView view;
        view.resize(500, 60);
        if (!view.load(makeEpub3(dir.path())))
            return qint64(1) << 40;
        view.show();
        QTest::qWait(10);
        view.setPagedModeForTest(true);

        QElapsedTimer timer;
        timer.start();
        view.turnPageForTest(1);
        return qint64(timer.elapsed());
    }, 3, 5);
    EXPECT_LT(turnMs, 5);
    fprintf(stderr,
            "[epub perf] page turn: %lld ms (a position update; painting the target page is a "
            "separate, worker-backed repaint - see PerfChapterRenderDispatchStaysOffTheUiThread)\n",
            (long long)turnMs);
}

TEST(EpubViewTest, PerfTocBuildOnAHugeToc)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    QTemporaryDir dir;
    EpubView view;
    view.resize(500, 400);
    ASSERT_TRUE(view.load(makeEpubWithHugeToc(dir.path(), 20, 100))); // 2000 entries

    QElapsedTimer timer;
    timer.start();
    view.setTocVisibleForTest(true); // top level only: 20 rows
    // Best of three, the repeats on a fresh view each: load only ever adds time.
    const qint64 firstShowMs = perf::bestOf([&, first = true]() mutable {
        if (std::exchange(first, false))
            return qint64(timer.elapsed());
        QTemporaryDir otherDir;
        EpubView other;
        other.resize(500, 400);
        if (!other.load(makeEpubWithHugeToc(otherDir.path(), 20, 100)))
            return qint64(1) << 40;
        QElapsedTimer again;
        again.start();
        other.setTocVisibleForTest(true);
        return qint64(again.elapsed());
    }, 3, 50);
    EXPECT_LT(firstShowMs, 50);

    timer.restart();
    view.expandTocForTest(0); // one volume's 100 rows
    const qint64 oneExpandMs = timer.elapsed();

    timer.restart();
    for (int i = 1; i < 20; ++i)
        view.expandTocForTest(i); // the other 19 volumes - what an eager build would have done at open()
    const qint64 restMs = timer.elapsed();

    fprintf(stderr,
            "[epub perf] TOC (2000 entries, 20 volumes x 100 chapters): open() pays 0 (deferred); "
            "first show (20 rows) %lld ms; one volume (100 rows) %lld ms; "
            "remaining 19 volumes (1900 rows, what eager open() used to pay) %lld ms\n",
            (long long)firstShowMs, (long long)oneExpandMs, (long long)restMs);
}

namespace {

/// A book of one chapter whose cover is a solid-colour PNG of the given size,
/// declared the EPUB 3 way (properties="cover-image") or the EPUB 2 way
/// (<meta name="cover">), or not at all.
enum class CoverKind { None, Property, Meta };

QString makeCoverBook(const QString &dir, CoverKind kind, QSize size = QSize(80, 120))
{
    QImage cover(size, QImage::Format_RGB32);
    cover.fill(QColor(200, 30, 30));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    cover.save(&buffer, "PNG");

    const QString item = kind == CoverKind::Property
        ? QStringLiteral("<item id='cv' href='img/cover.png' media-type='image/png' properties='cover-image'/>")
        : QStringLiteral("<item id='cv' href='img/cover.png' media-type='image/png'/>");
    const QString meta = kind == CoverKind::Meta ? QStringLiteral("<meta name='cover' content='cv'/>") : QString();
    const QByteArray opf = QStringLiteral(
        "<?xml version='1.0'?><package xmlns='http://www.idpf.org/2007/opf' version='3.0'>"
        "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:title>Covered</dc:title>%1</metadata>"
        "<manifest><item id='c1' href='ch1.xhtml' media-type='application/xhtml+xml'/>%2</manifest>"
        "<spine><itemref idref='c1'/></spine></package>").arg(meta, item).toUtf8();
    const QString path = QDir(dir).filePath(QStringLiteral("cover%1.epub").arg(int(kind)));
    writeZip(path, {{QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
                    {QStringLiteral("META-INF/container.xml"), QByteArray(kContainerXML)},
                    {QStringLiteral("OEBPS/content.opf"), opf},
                    {QStringLiteral("OEBPS/ch1.xhtml"),
                     QByteArrayLiteral("<html xmlns='http://www.w3.org/1999/xhtml'><body><p>x</p></body></html>")},
                    {QStringLiteral("OEBPS/img/cover.png"), png}});
    return path;
}

} // namespace

TEST(EpubCoverTest, FoundByManifestPropertyOrByMetaAndScaledDown)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const CoverKind kind : {CoverKind::Property, CoverKind::Meta}) {
        const QImage cover = noteAppC::epubCover(makeCoverBook(dir.path(), kind));
        ASSERT_FALSE(cover.isNull()) << int(kind);
        EXPECT_EQ(cover.size(), QSize(80, 120));
        EXPECT_EQ(cover.pixelColor(10, 10), QColor(200, 30, 30));
    }
    // A cover bigger than the bound keeps its proportions inside it.
    const QImage big = noteAppC::epubCover(makeCoverBook(dir.path(), CoverKind::Property, QSize(600, 900)), 256);
    EXPECT_EQ(big.height(), 256);
    EXPECT_NEAR(big.width(), 171, 1);
}

TEST(EpubCoverTest, NoCoverNotAnEpubOrAMissingFileGiveANullImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_TRUE(noteAppC::epubCover(makeCoverBook(dir.path(), CoverKind::None)).isNull());
    EXPECT_TRUE(noteAppC::epubCover(makeEpub3(dir.path())).isNull()) << "declares no cover";
    const QString text = dir.filePath(QStringLiteral("x.epub"));
    QFile file(text);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not a zip");
    file.close();
    EXPECT_TRUE(noteAppC::epubCover(text).isNull());
    EXPECT_TRUE(noteAppC::epubCover(dir.filePath(QStringLiteral("gone.epub"))).isNull());
}

TEST(EpubCoverTest, TheEmbedCardShowsTheCoverOnceItIsRead)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString withCover = makeCoverBook(dir.path(), CoverKind::Property);
    const QString without = makeCoverBook(dir.path(), CoverKind::None);

    noteAppC::MarkdownEditor editor;
    editor.resize(900, 600);
    editor.setLinkResolver([&](const QString &name) { return name.contains(QLatin1String("plain")) ? without : withCover; });
    editor.setNoteText(QStringLiteral("![[book.epub]]\n"));
    // A card at once, its cover when the worker has read it.
    EXPECT_TRUE(editor.isFileCardAt(0));
    const qreal plainHeight = editor.documentHeight();
    WAIT_TRUE(editor.fileCardHasCoverAt(0), 5000);
    EXPECT_GT(editor.documentHeight(), plainHeight) << "the card grows to hold a portrait";

    noteAppC::MarkdownEditor bare;
    bare.resize(900, 600);
    bare.setLinkResolver([&](const QString &) { return without; });
    bare.setNoteText(QStringLiteral("![[plain.epub]]\n"));
    EXPECT_TRUE(bare.isFileCardAt(0));
    QTest::qWait(300); // the read of a book with no cover must not conjure one
    EXPECT_FALSE(bare.fileCardHasCoverAt(0));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // EpubPositionStore and EpubView::cacheRootForBook() both fall back to
    // the real $HOME/.config, $HOME/.cache when XDG_* is unset - this suite
    // never touches the real home (see the header comment).
    QTemporaryDir xdgHome;
    qputenv("XDG_CONFIG_HOME", xdgHome.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", xdgHome.filePath(QStringLiteral("cache")).toUtf8());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
