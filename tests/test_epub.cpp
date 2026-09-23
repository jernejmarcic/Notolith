// EPUB reader: archive access, EPUB2 (NCX) and EPUB3 (nav) parsing, spine
// order, the archive-path sandbox (../ escapes and remote URLs refused),
// script stripping, a highlight locator's round trip through the sidecar,
// the sidecar keeping the reader's own note text across re-extraction, and
// that a render scheduled before the view is destroyed does nothing after.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <archive.h>
#include <archive_entry.h>

#include "EpubArchive.h"
#include "EpubDocument.h"
#include "EpubHighlights.h"
#include "EpubView.h"
#include "HtmlRenderer.h"
#include "Vault.h"

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
    // silent either way, which is why HANDOFF.md calls for an ASAN pass on
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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
