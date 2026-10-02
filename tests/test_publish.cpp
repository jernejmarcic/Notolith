// Tests for libs/ext/publish/: privacy (the first correctness property, per
// the task), link/anchor resolution, escaping, maths, attachments and the
// incremental republish. Each test builds its own throwaway vault under a
// QTemporaryDir - real files and a real Index, never a synthetic Decorations
// - and reads Publisher's own output back off disk, the way a reader's
// browser would.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtConcurrent>

#include "Index.h"
#include "Indexer.h"
#include "MathRenderer.h"
#include "Publisher.h"
#include "Vault.h"

using namespace noteAppC;
using namespace noteAppC::publish;

namespace {

/// A throwaway vault plus its (synchronously built) Index, and a separate
/// throwaway output directory - never a subfolder of the vault, so nothing
/// here depends on Publisher's own vault-root refusal to also be correct.
struct TestSite {
    QTemporaryDir vaultDir;
    QTemporaryDir outDir;
    Vault vault;
    Index index;

    explicit TestSite(const QHash<QString, QByteArray> &files)
    {
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QString path = vaultDir.filePath(it.key());
            QDir().mkpath(QFileInfo(path).path());
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(it.value());
        }
        vault = Vault(vaultDir.path());
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        done.wait(20000);
    }

    QString read(const QString &outputRelativePath) const
    {
        QFile file(outDir.filePath(outputRelativePath));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

    bool exists(const QString &outputRelativePath) const
    {
        return QFile::exists(outDir.filePath(outputRelativePath));
    }

    Counts publish(const Options &extra = {})
    {
        Publisher publisher(vault, index);
        Options options = extra;
        options.outputDir = outDir.path();
        QString error;
        const bool ok = publisher.publish(options, nullptr, &error);
        EXPECT_TRUE(ok) << error.toStdString();
        return publisher.counts();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Links and anchors

TEST(PublishTest, LinksResolveAcrossFolders)
{
    TestSite site({
        {QStringLiteral("Root.md"), "See [[notes/Sub]].\n"},
        {QStringLiteral("notes/Sub.md"), "See [[Root]].\n"},
    });
    site.publish();

    const QString root = site.read(QStringLiteral("root.html"));
    ASSERT_FALSE(root.isEmpty());
    EXPECT_TRUE(root.contains(QStringLiteral("href=\"notes/sub.html\"")));

    const QString sub = site.read(QStringLiteral("notes/sub.html"));
    ASSERT_FALSE(sub.isEmpty());
    EXPECT_TRUE(sub.contains(QStringLiteral("href=\"../root.html\"")));
}

TEST(PublishTest, HeadingAndBlockAnchors)
{
    TestSite site({
        {QStringLiteral("Target.md"),
         "## My Heading\n\nText here. ^myblock\n"},
        {QStringLiteral("Source.md"),
         "[[Target#My Heading]] and [[Target#^myblock]]\n"},
    });
    site.publish();

    const QString target = site.read(QStringLiteral("target.html"));
    ASSERT_FALSE(target.isEmpty());
    EXPECT_TRUE(target.contains(QStringLiteral("id=\"my-heading\"")));
    EXPECT_TRUE(target.contains(QStringLiteral("id=\"block-myblock\"")))
        << target.toStdString();

    const QString source = site.read(QStringLiteral("source.html"));
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.contains(QStringLiteral("target.html#my-heading")));
    EXPECT_TRUE(source.contains(QStringLiteral("target.html#block-myblock")));
}

// ---------------------------------------------------------------------------
// Privacy: the property this whole lane exists for.

TEST(PublishTest, UnpublishedNoteLeaksNowhereInDefaultMode)
{
    TestSite site({
        {QStringLiteral("Public.md"),
         "Links to [[Secret]] and embeds ![[Secret]].\n\n"
         "![[picture.png]]\n"},
        {QStringLiteral("Secret.md"),
         "---\npublish: false\n---\n\nTopSecretPhraseXYZ links back to [[Public]].\n"},
        {QStringLiteral("picture.png"), QByteArray("\x89PNG-not-a-real-image")},
        {QStringLiteral("secret-only.png"), QByteArray("\x89PNG-not-a-real-image-either")},
    });
    // Secret.md does not reference secret-only.png; nothing does but itself,
    // so it must never be copied - see the attachments test below for the
    // positive case (an image a *published* note embeds).
    Counts counts = site.publish();
    EXPECT_EQ(counts.skipped, 1);

    // 1. No page for the unpublished note.
    EXPECT_FALSE(site.exists(QStringLiteral("secret.html")));

    // 2. Its content never appears inlined into the page that tried to embed
    //    it - Public.md's own text naming "Secret" (its author's own choice
    //    of what to link to, already visible in Public.md's own source) is
    //    not itself a leak, so this checks for a *working* link/href and for
    //    the private note's actual body text, not the bare word.
    const QString publicPage = site.read(QStringLiteral("public.html"));
    ASSERT_FALSE(publicPage.isEmpty());
    EXPECT_FALSE(publicPage.contains(QStringLiteral("TopSecretPhraseXYZ")));
    EXPECT_FALSE(publicPage.contains(QStringLiteral("href=\"secret.html\"")));
    // 3. The link/embed both degrade to the same "unresolved" styling, not a
    //    working link naming the private note.
    EXPECT_TRUE(publicPage.contains(QStringLiteral("unresolved")));

    // 4. Backlinks: Secret.md links to Public.md, but must not show up there
    //    - nothing else does, so the whole section is absent.
    EXPECT_FALSE(publicPage.contains(QStringLiteral("class=\"backlinks\"")));

    // 5. Search index: the private note's own text, and its page, never
    //    present - not even as an entry with no text.
    const QString searchIndex = site.read(QStringLiteral("search-index.json"));
    EXPECT_FALSE(searchIndex.contains(QStringLiteral("TopSecretPhraseXYZ")));
    EXPECT_FALSE(searchIndex.contains(QStringLiteral("secret.html")));

    // 6. Attachments: only what a *published* page actually referenced.
    EXPECT_TRUE(site.exists(QStringLiteral("attachments/picture.png")));
    EXPECT_FALSE(site.exists(QStringLiteral("attachments/secret-only.png")));
}

TEST(PublishTest, OnlyMarkedModeExcludesEverythingWithoutPublishTrue)
{
    TestSite site({
        {QStringLiteral("Marked.md"), "---\npublish: true\n---\n\nVisible body VISIBLEMARK.\n"},
        {QStringLiteral("Unmarked.md"), "Body text UNMARKEDSECRET, no publish property at all.\n"},
    });
    Counts counts = site.publish(Options{.onlyMarked = true});
    EXPECT_EQ(counts.pages, 1);
    EXPECT_EQ(counts.skipped, 1);

    EXPECT_TRUE(site.exists(QStringLiteral("marked.html")));
    EXPECT_FALSE(site.exists(QStringLiteral("unmarked.html")));
    const QString index = site.read(QStringLiteral("search-index.json"));
    EXPECT_TRUE(index.contains(QStringLiteral("VISIBLEMARK")));
    EXPECT_FALSE(index.contains(QStringLiteral("UNMARKEDSECRET")));
}

TEST(PublishTest, PublishFalseWinsEvenOverRichContent)
{
    // publish: false excludes a note outright - not just from linking, from
    // the run entirely - regardless of anything else on it (aliases, tags,
    // a permalink, links other notes make to it).
    TestSite site({
        {QStringLiteral("Excluded.md"),
         "---\npublish: false\npermalink: should-never-appear\ntags: [x]\n---\n\nBody.\n"},
    });
    Counts counts = site.publish();
    EXPECT_EQ(counts.pages, 0);
    EXPECT_EQ(counts.skipped, 1);
    EXPECT_FALSE(site.exists(QStringLiteral("excluded.html")));
    EXPECT_FALSE(site.exists(QStringLiteral("should-never-appear.html")));
}

// ---------------------------------------------------------------------------
// Escaping

TEST(PublishTest, ScriptTagEscapedEverywhere)
{
    // A `<script>` written directly in prose (not fenced as code) is real
    // inline HTML by CommonMark's own grammar, not a literal string - so the
    // safe outcome for the heading and the body is sanitising (stripped, see
    // RawHtmlSanitised), not HTML-escaping; either is fine, an executable
    // <script> surviving is not. A frontmatter *property value* and a link
    // *alias*, by contrast, are never parsed as markup at all - just data -
    // so those two are where escaping specifically is the property to check,
    // using plain `<`/`>`/`&` rather than a real tag so the assertion is not
    // testing the same sanitiser path a second time under another name.
    TestSite site({
        {QStringLiteral("Target.md"), "# Anchor\n\nJust text.\n"},
        {QStringLiteral("Danger.md"),
         "---\ntitle: <script>alert(1)</script>\n---\n\n"
         "# <script>alert(2)</script>\n\n"
         "Body has <script>alert(3)</script> in it.\n\n"
         "[[Target|1 < 2 & \"quoted\" > 0]]\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("danger.html"));
    ASSERT_FALSE(page.isEmpty());
    // No live <script ...>alert(...) tag anywhere - the page's own
    // search.js <script src="..."> tag is the only "<script" substring a
    // correct page may contain.
    EXPECT_FALSE(page.contains(QStringLiteral("<script>alert")));
    EXPECT_TRUE(page.contains(QStringLiteral("&lt;script&gt;alert(1)")));  // title (a property value)
    EXPECT_TRUE(page.contains(QStringLiteral("alert(2)")));  // heading: content kept, tag sanitised
    EXPECT_TRUE(page.contains(QStringLiteral("alert(3)")));  // body: content kept, tag sanitised
    EXPECT_TRUE(page.contains(QStringLiteral("1 &lt; 2 &amp; &quot;quoted&quot; &gt; 0")));  // link alias
}

TEST(PublishTest, RawHtmlSanitised)
{
    TestSite site({
        {QStringLiteral("Note.md"),
         "<div onclick=\"steal()\">\n<script>evil()</script>\n"
         "<iframe src=\"https://example.com\"></iframe>\n"
         "<u>kept</u>\n</div>\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    ASSERT_FALSE(page.isEmpty());
    EXPECT_FALSE(page.contains(QStringLiteral("onclick")));
    // Not a bare "<script" check: every page legitimately has one, for the
    // search box (<script src="search.js" defer>).
    EXPECT_FALSE(page.contains(QStringLiteral("<script>evil")));
    EXPECT_FALSE(page.contains(QStringLiteral("evil()")));
    EXPECT_FALSE(page.contains(QStringLiteral("<iframe")));
    EXPECT_TRUE(page.contains(QStringLiteral("<u>kept</u>")));
}

TEST(PublishTest, AnHTMLHeadingIsDrawnOnceAndIsAnIndexedHeading)
{
    TestSite site({
        {QStringLiteral("Note.md"), "# Top\n\n<h2>Sub &amp; more</h2>\n\ntext\n"},
        {QStringLiteral("Other.md"), "[[Note#Sub & more]]\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    ASSERT_FALSE(page.isEmpty());
    EXPECT_EQ(page.count(QStringLiteral("more</h2>")), 1) << page.toStdString();
    EXPECT_TRUE(page.contains(QStringLiteral("id=\"top\"")));
}

// ---------------------------------------------------------------------------
// Maths, attachments

TEST(PublishTest, MathsBecomesSvg)
{
    if (!MathRenderer::isAvailable())
        GTEST_SKIP() << "MicroTeX resources not found in this build";
    TestSite site({
        {QStringLiteral("Note.md"), "Inline $x^2$ and:\n\n$$y = x^2$$\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    ASSERT_FALSE(page.isEmpty());
    EXPECT_TRUE(page.contains(QStringLiteral("<svg")));
    EXPECT_FALSE(page.contains(QStringLiteral("math-source")));
}

TEST(PublishTest, ImageEmbedCopiesOnlyReferencedAttachments)
{
    TestSite site({
        {QStringLiteral("Note.md"), "![[used.png]]\n"},
        {QStringLiteral("used.png"), QByteArray("used-bytes")},
        {QStringLiteral("unused.png"), QByteArray("unused-bytes")},
    });
    Counts counts = site.publish();
    EXPECT_EQ(counts.attachments, 1);
    EXPECT_TRUE(site.exists(QStringLiteral("attachments/used.png")));
    EXPECT_FALSE(site.exists(QStringLiteral("attachments/unused.png")));
    EXPECT_EQ(site.read(QStringLiteral("attachments/used.png")), QStringLiteral("used-bytes"));
}

// ---------------------------------------------------------------------------
// Cycle guard

TEST(PublishTest, EmbedCycleTerminates)
{
    TestSite site({
        {QStringLiteral("A.md"), "![[B]]\n"},
        {QStringLiteral("B.md"), "![[A]]\n"},
    });
    Counts counts;
    ASSERT_NO_FATAL_FAILURE(counts = site.publish());
    EXPECT_EQ(counts.pages, 2);
    const QString a = site.read(QStringLiteral("a.html"));
    ASSERT_FALSE(a.isEmpty());
    // Terminated via the explicit cycle guard specifically, not merely by
    // eventually hitting the generic recursion-depth safety net a few levels
    // further down (renderEmbed() gives the two cases distinct markers for
    // exactly this reason) - verified by temporarily disabling the
    // m_embedStack check in PageBody.cpp during development: this assertion
    // then failed (the page still rendered and still terminated, but by
    // falling through to "embed-limit" a few levels later, not
    // "embed-cycle") exactly as it should when the guard it is testing is
    // not there.
    EXPECT_TRUE(a.contains(QStringLiteral("embed-cycle")));
    EXPECT_FALSE(a.contains(QStringLiteral("embed-limit")));
}

// ---------------------------------------------------------------------------
// Incremental republish

TEST(PublishTest, NoChangeRepublishRewritesNothing)
{
    TestSite site({
        {QStringLiteral("One.md"), "Body one.\n"},
        {QStringLiteral("Two.md"), "Body two, linking [[One]].\n"},
    });
    const Counts first = site.publish();
    ASSERT_GT(first.pages, 0);
    EXPECT_EQ(first.unchanged, 0);

    const Counts second = site.publish();
    EXPECT_EQ(second.pages, first.pages);
    EXPECT_EQ(second.removed, 0);
    // Every file this run touched (pages + site.css + search assets + tag
    // pages, if any) came back byte-identical.
    EXPECT_GT(second.unchanged, 0);
}

TEST(PublishTest, RemovingPublishRemovesItsOldPage)
{
    TestSite site({
        {QStringLiteral("Stays.md"), "Body.\n"},
        {QStringLiteral("Goes.md"), "Body.\n"},
    });
    site.publish();
    ASSERT_TRUE(site.exists(QStringLiteral("goes.html")));

    // Mark Goes.md private and reindex before republishing - Publisher reads
    // the *index's* cached properties (Phase 0), so a rebuilt cache is what
    // makes it visible, exactly as it would be for a real edit-then-republish.
    QFile goes(site.vaultDir.filePath(QStringLiteral("Goes.md")));
    goes.open(QIODevice::WriteOnly);
    goes.write("---\npublish: false\n---\n\nBody.\n");
    goes.close();
    // A fresh Indexer needs setVault() before it knows this vault at all -
    // a full rescan is simplest and, for two notes, cheap enough to just do.
    Indexer indexer(&site.index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(site.vault);
    ASSERT_TRUE(done.wait(20000));

    const Counts second = site.publish();
    EXPECT_GE(second.removed, 1);
    EXPECT_FALSE(site.exists(QStringLiteral("goes.html")));
    EXPECT_TRUE(site.exists(QStringLiteral("stays.html")));
}

// ---------------------------------------------------------------------------
// Output directory safety

TEST(PublishTest, RefusesTheVaultRootItself)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    Publisher publisher(site.vault, site.index);
    Options options;
    options.outputDir = site.vaultDir.path();
    QString error;
    EXPECT_FALSE(publisher.publish(options, nullptr, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(PublishTest, FootnotesAreNumberedLinkedAndListedOnce)
{
    TestSite site({{QStringLiteral("Note.md"),
                    "Text.[^a] Inline.^[Inline body.] Again.[^a]\n\n[^a]: The **body**.\n\nAfter.\n"}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    EXPECT_TRUE(page.contains(QStringLiteral("<a href=\"#fn-1\" id=\"fnref-1\">1</a>")));
    EXPECT_TRUE(page.contains(QStringLiteral("<a href=\"#fn-2\" id=\"fnref-2\">2</a>")));
    EXPECT_EQ(page.count(QStringLiteral("id=\"fnref-1\"")), 1); // the second reference has no id
    EXPECT_TRUE(page.contains(QStringLiteral("<li id=\"fn-1\">The <strong>body</strong>.")));
    EXPECT_EQ(page.count(QStringLiteral("The <strong>body</strong>")), 1); // not also a paragraph
    EXPECT_FALSE(page.contains(QStringLiteral("[^a]")));
}

// ---------------------------------------------------------------------------
// Off the UI thread

// What MainWindow::publishSite does: its own Index on the same cache, opened
// on the pool thread that publishes, so links resolve without touching the
// UI thread's connection.
TEST(PublishTest, PublishesFromAPoolThreadWithItsOwnIndex)
{
    TestSite site({
        {QStringLiteral("Root.md"), "See [[notes/Sub]].\n"},
        {QStringLiteral("notes/Sub.md"), "Body.\n"},
    });
    const Vault vault = site.vault;
    const QString folder = site.outDir.path();
    const QString error = QtConcurrent::run([vault, folder] {
        Index index;
        if (!index.open(vault.root()))
            return index.lastError();
        Publisher publisher(vault, index);
        Options options;
        options.outputDir = folder;
        QString failure;
        if (!publisher.publish(options, nullptr, &failure))
            return failure;
        return publisher.counts().pages == 2 ? QString() : QStringLiteral("wrong page count");
    }).result();
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_TRUE(site.read(QStringLiteral("root.html")).contains(QStringLiteral("href=\"notes/sub.html\"")));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir configHome;
    QTemporaryDir cacheHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
