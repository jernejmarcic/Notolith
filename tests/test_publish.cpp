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
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtConcurrent>

#include "CodeHighlighter.h"
#include "Index.h"
#include "Indexer.h"
#include "MathRenderer.h"
#include "MermaidCache.h"
#include "PageBody.h"
#include "Parser.h"
#include "Publisher.h"
#include "Theme.h"
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

TEST(PublishTest, TheSchemesCodeColoursReachThePage)
{
    TestSite site({{QStringLiteral("Code.md"), "```cpp\nstatic int x = 0;\n```\n"}});
    site.publish();
    EXPECT_FALSE(site.read(QStringLiteral("code.html")).contains(QLatin1String("--code-type")))
        << "the built-in scheme names no token colours";

    Options options;
    options.codeThemeDark = Theme::builtin(Theme::Mode::Dark);
    options.codeThemeDark->setToken(QStringLiteral("--syntax-type"), QStringLiteral("#C678DD bold"));
    options.codeThemeLight = Theme::builtin(Theme::Mode::Light);
    options.codeThemeLight->setToken(QStringLiteral("--syntax-type"), QStringLiteral("#7A3E9D"));
    site.publish(options); // the same process again: a pool thread must not keep the first run's theme
    const QString page = site.read(QStringLiteral("code.html"));
    EXPECT_TRUE(page.contains(QLatin1String("--code-type:#c678dd"))) << page.toStdString();
    EXPECT_TRUE(page.contains(QLatin1String("--code-type:#7a3e9d")));
    EXPECT_TRUE(page.contains(QLatin1String("tok-")));
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

TEST(PublishTest, ANestedHeadingLinkPointsAtTheHeadingUnderItsParent)
{
    TestSite site({
        {QStringLiteral("Target.md"), "# One\n\n## Detail\n\n# Two\n\n## Detail\n\n## Other\n"},
        {QStringLiteral("Source.md"), "[[Target#One#Detail]] [[Target#Two#Detail]] [[Target#Two#Other]]\n"},
    });
    site.publish();
    const QString source = site.read(QStringLiteral("source.html"));
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.contains(QStringLiteral("target.html#detail\""))) << source.toStdString();
    EXPECT_TRUE(source.contains(QStringLiteral("target.html#detail-2\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("target.html#other\"")));
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

// The SVGs of the page's diagrams, decoded from their <img> data URIs.
QStringList diagramImages(const QString &page)
{
    static const QRegularExpression image(QStringLiteral("<div class=\"diagram\"><img alt=\"[^\"]*\" src=\"data:image/svg\\+xml;base64,([A-Za-z0-9+/=]*)\">"));
    QStringList svgs;
    for (auto it = image.globalMatch(page); it.hasNext();)
        svgs << QString::fromUtf8(QByteArray::fromBase64(it.next().captured(1).toLatin1()));
    return svgs;
}

// A note can put any markup into its SVG (TikZ writes \special{dvisvgm:raw}
// straight through): a published diagram is an <img>, where script never runs.
TEST(PublishTest, ADiagramsOwnMarkupNeverReachesThePage)
{
    QTemporaryDir env;
    ASSERT_TRUE(env.isValid());
    const QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", env.filePath(QStringLiteral("cache")).toUtf8());
    const QString source = QStringLiteral("\\draw (0,0);");
    ASSERT_TRUE(mermaidCacheStore(
        noteAppC::diagramCachePath(noteAppC::DiagramKind::TikZ, source,
                                   noteAppC::diagramConfig(noteAppC::DiagramKind::TikZ, Theme::builtin(Theme::Mode::Dark))),
        "<svg xmlns='http://www.w3.org/2000/svg' onload='alert(1)'><a href='&#106;avascript:alert(2)'><text>X</text></a></svg>"));
    TestSite site({{QStringLiteral("Note.md"), "```tikz\n\\draw (0,0);\n```\n"}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    if (oldCache.isNull())
        qunsetenv("XDG_CACHE_HOME");
    else
        qputenv("XDG_CACHE_HOME", oldCache);
    EXPECT_EQ(diagramImages(page).size(), 1) << page.toStdString();
    EXPECT_FALSE(page.contains(QStringLiteral("onload")));
    EXPECT_FALSE(page.contains(QStringLiteral("avascript")));
    // The class is the kind-neutral `diagram`, and the stylesheet has it.
    const QString css = site.read(QStringLiteral("site.css"));
    EXPECT_TRUE(css.contains(QStringLiteral(".diagram img")));
    EXPECT_FALSE(css.contains(QStringLiteral("mermaid-diagram")));
}

// The picture's words, for a screen reader and the site's search: the source is the alt text, escaped (a
// source is a stranger's input: it cannot close the attribute) and capped.
TEST(PublishTest, ADiagramsSourceIsItsAltTextEscapedAndCapped)
{
    QTemporaryDir env;
    ASSERT_TRUE(env.isValid());
    const QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", env.filePath(QStringLiteral("cache")).toUtf8());
    using noteAppC::DiagramKind;
    const Theme dark = Theme::builtin(Theme::Mode::Dark);
    const QString evil = QStringLiteral("digraph{ \"><script>alert(1)</script> a->b }");
    const QString longer = QStringLiteral("digraph{ ") + QString(3000, QLatin1Char('x')) + QStringLiteral(" a->b }");
    for (const QString &source : {evil, longer})
        ASSERT_TRUE(mermaidCacheStore(noteAppC::diagramCachePath(DiagramKind::Graphviz, source, noteAppC::diagramConfig(DiagramKind::Graphviz, dark)),
                                      "<svg xmlns=\"http://www.w3.org/2000/svg\"><text>X</text></svg>"));
    TestSite site({{QStringLiteral("Note.md"), ("```dot\n" + evil + "\n```\n\n```dot\n" + longer + "\n```\n").toUtf8()}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    if (oldCache.isNull())
        qunsetenv("XDG_CACHE_HOME");
    else
        qputenv("XDG_CACHE_HOME", oldCache);
    ASSERT_EQ(diagramImages(page).size(), 2) << page.toStdString();
    EXPECT_TRUE(page.contains(QStringLiteral("<img alt=\"digraph{ &quot;&gt;&lt;script&gt;alert(1)&lt;/script&gt; a-&gt;b }\" src="))) << page.toStdString();
    EXPECT_FALSE(page.contains(QStringLiteral("<script>alert(1)")));
    EXPECT_TRUE(page.contains(QStringLiteral("<img alt=\"digraph{ ") + QString(1991, QLatin1Char('x')) + QStringLiteral("\" src=")));
    EXPECT_FALSE(page.contains(QString(1992, QLatin1Char('x')))) << "capped at 2000 characters of source";
}

// A fence the editor drew is put in from the cache; publishing never runs mmdr
// (a stub first on PATH that would leave a file if it did).
TEST(PublishTest, AMermaidFenceBecomesTheCachedSVGAndNeverStartsTheTool)
{
    QTemporaryDir env;
    ASSERT_TRUE(env.isValid());
    const QByteArray oldCache = qgetenv("XDG_CACHE_HOME"), oldPath = qgetenv("PATH");
    qputenv("XDG_CACHE_HOME", env.filePath(QStringLiteral("cache")).toUtf8());
    const QString ran = env.filePath(QStringLiteral("ran"));
    QDir().mkpath(env.filePath(QStringLiteral("bin")));
    QFile stub(env.filePath(QStringLiteral("bin/mmdr")));
    ASSERT_TRUE(stub.open(QIODevice::WriteOnly));
    stub.write(("#!/bin/sh\necho ran > '" + ran + "'\n").toUtf8());
    stub.close();
    QFile::setPermissions(stub.fileName(), QFile::permissions(stub.fileName()) | QFile::ExeOwner);
    qputenv("PATH", env.filePath(QStringLiteral("bin")).toUtf8() + ":" + oldPath);

    // What the editor stored for this source: the fence content, as the editor asks for it.
    const QString drawn = QStringLiteral("graph TD\nA-->B");
    ASSERT_TRUE(mermaidCacheStore(
        mermaidCachePath(drawn, mermaidConfig(Theme::builtin(Theme::Mode::Dark))),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><defs><marker id=\"arrow-0\"/></defs>"
        "<text fill=\"url(#arrow-0)\">DRAWNMARK</text></svg>"));
    TestSite site({
        {QStringLiteral("Note.md"), "```mermaid\ngraph TD\nA-->B\n```\n\n```mermaid\ngraph TD\nNEVERDRAWN-->B\n```\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    qputenv("PATH", oldPath);
    if (oldCache.isNull())
        qunsetenv("XDG_CACHE_HOME");
    else
        qputenv("XDG_CACHE_HOME", oldCache);

    const QStringList images = diagramImages(page);
    ASSERT_EQ(images.size(), 1) << page.toStdString();
    EXPECT_TRUE(images.first().contains(QStringLiteral("DRAWNMARK")));
    // Never drawn in the editor: its source, as an ordinary code block.
    EXPECT_TRUE(page.contains(QStringLiteral("NEVERDRAWN")));
    EXPECT_TRUE(page.contains(QStringLiteral("language-mermaid")));
    EXPECT_FALSE(QFile::exists(ran)) << "publish started mmdr";
}

// dot and dvisvgm start their SVG with an XML prolog and dvisvgm quotes with ': the prolog is
// dropped and the ids are still made page-unique; a tikz fence the editor never drew stays its listing.
TEST(PublishTest, DotAndTikZFencesBecomeTheirCachedSVGs)
{
    QTemporaryDir env;
    ASSERT_TRUE(env.isValid());
    const QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", env.filePath(QStringLiteral("cache")).toUtf8());
    const Theme dark = Theme::builtin(Theme::Mode::Dark);
    using noteAppC::DiagramKind;
    ASSERT_TRUE(mermaidCacheStore(
        noteAppC::diagramCachePath(DiagramKind::Graphviz, QStringLiteral("digraph{a->b}"), noteAppC::diagramConfig(DiagramKind::Graphviz, dark)),
        "<?xml version=\"1.0\"?>\n<!DOCTYPE svg>\n<svg xmlns=\"http://www.w3.org/2000/svg\"><g id=\"graph0\"><text>DOTMARK</text></g></svg>"));
    ASSERT_TRUE(mermaidCacheStore(
        noteAppC::diagramCachePath(DiagramKind::TikZ, QStringLiteral("\\draw (0,0);"), noteAppC::diagramConfig(DiagramKind::TikZ, dark)),
        "<?xml version='1.0'?>\n<svg xmlns='http://www.w3.org/2000/svg'><defs><path id='g0-65' d='M0 0'/></defs>"
        "<use xlink:href='#g0-65'/></svg>"));
    TestSite site({
        {QStringLiteral("Note.md"), "```dot\ndigraph{a->b}\n```\n\n```tikz\n\\draw (0,0);\n```\n\n```plantuml\nNEVERDRAWN -> B\n```\n"},
    });
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    if (oldCache.isNull())
        qunsetenv("XDG_CACHE_HOME");
    else
        qputenv("XDG_CACHE_HOME", oldCache);

    const QStringList images = diagramImages(page);
    ASSERT_EQ(images.size(), 2) << page.toStdString();
    EXPECT_TRUE(images.at(0).contains(QStringLiteral("DOTMARK")));
    EXPECT_TRUE(images.at(1).contains(QStringLiteral("g0-65")));
    EXPECT_TRUE(page.contains(QStringLiteral("NEVERDRAWN")));
    EXPECT_TRUE(page.contains(QStringLiteral("language-plantuml")));
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

TEST(PublishTest, RepeatedFootnoteReferencesGetUniqueIdsAndBackLinks)
{
    TestSite site({{QStringLiteral("Note.md"), "One.[^a] Two.[^a] Three.[^a]\n\n[^a]: Body.\n"}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    EXPECT_EQ(page.count(QStringLiteral("id=\"fnref-1\"")), 1);
    EXPECT_EQ(page.count(QStringLiteral("id=\"fnref-1-2\"")), 1);
    EXPECT_EQ(page.count(QStringLiteral("id=\"fnref-1-3\"")), 1);
    // One way back per reference, as a text arrow with an accessible name.
    EXPECT_EQ(page.count(QStringLiteral("class=\"footnote-back\"")), 3);
    EXPECT_TRUE(page.contains(QStringLiteral("href=\"#fnref-1-3\" aria-label=\"Back to reference 1 (3)\"")));
    EXPECT_TRUE(page.contains(QStringLiteral("&#8617;&#xFE0E;")));
}

TEST(PublishTest, ConsecutiveTabFencesBecomeANoJavaScriptTabStrip)
{
    TestSite site({{QStringLiteral("Note.md"),
                    "```python tab\nprint(1)\n```\n\n```cpp tab=\"C++ <b>\"\nint x;\n```\n\n"
                    "```sh\nls\n```\n\n```a tab\n1\n```\n```b tab\n2\n```\n"}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    // Two groups, numbered apart; the plain fence between them stays a plain block.
    EXPECT_EQ(page.count(QStringLiteral("class=\"code-tabs\"")), 2);
    EXPECT_TRUE(page.contains(QStringLiteral("name=\"code-tabs-1\" id=\"code-tabs-1-1\" checked>")));
    EXPECT_TRUE(page.contains(QStringLiteral("name=\"code-tabs-1\" id=\"code-tabs-1-2\">")));
    EXPECT_EQ(page.count(QStringLiteral(" checked>")), 2); // first tab of each group only
    // A bare `tab` is labelled by its language; a named one is escaped.
    EXPECT_TRUE(page.contains(QStringLiteral("for=\"code-tabs-1-1\">python</label>")));
    EXPECT_TRUE(page.contains(QStringLiteral("for=\"code-tabs-1-2\">C++ &lt;b&gt;</label>")));
    EXPECT_EQ(page.count(QStringLiteral("class=\"code-block")), 5);
    // Nothing script-driven inside the strip itself (the page has the site's own search script).
    const qsizetype stripStart = page.indexOf(QStringLiteral("class=\"code-tabs\""));
    EXPECT_FALSE(page.mid(stripStart, page.indexOf(QStringLiteral("language-sh"), stripStart) - stripStart)
                     .contains(QStringLiteral("<script")));
    EXPECT_TRUE(site.read(QStringLiteral("site.css")).contains(QStringLiteral(".code-tab-input:checked")));
}

TEST(PublishTest, ALoneTabFenceIsAnOrdinaryBlock)
{
    TestSite site({{QStringLiteral("Note.md"), "```python tab\nprint(1)\n```\n"}});
    site.publish();
    EXPECT_FALSE(site.read(QStringLiteral("note.html")).contains(QStringLiteral("code-tabs")));
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

TEST(ComplexCodeTest, PublishEmitsTokenClasses)
{
    const QString text = QStringLiteral("```cpp\nstatic int x = 0;\nreturn x;\n```\n");
    ParsedNote note;
    note.relativePath = QStringLiteral("a.md");
    note.outputPath = QStringLiteral("a.html");
    note.title = QStringLiteral("a");
    note.text = text;
    note.decorations = Parser().parseAll(text).decorations;
    const PublishedNotes notes;
    const QHash<QString, QString> resolved;

    const auto render = [&](const Theme &dark) {
        noteAppC::CodeHighlighter highlighter;
        highlighter.setTheme(dark);
        const Theme light = Theme::builtin(Theme::Mode::Light);
        const RenderEnv env{notes, resolved, highlighter, nullptr, dark, light, 4};
        return PageBody::render(note, env);
    };

    // A theme that names token colours: the variables, the rules, the classes.
    Theme themed = Theme::builtin(Theme::Mode::Dark);
    themed.setToken(QStringLiteral("--syntax-keyword"), QStringLiteral("#C678DD bold"));
    themed.setToken(QStringLiteral("--syntax-type"), QStringLiteral("#61AFEF"));
    const ContentResult styled = render(themed);
    EXPECT_TRUE(styled.codeStyle.contains(QLatin1String("--code-keyword:#c678dd")));
    EXPECT_TRUE(styled.codeStyle.contains(QLatin1String(".tok-type{color:var(--code-type);}")));
    // `return` is a keyword.control: its own class, the `keyword` variable.
    EXPECT_TRUE(styled.codeStyle.contains(QLatin1String(
        ".tok-keyword-control{color:var(--code-keyword);font-weight:600;}")));
    EXPECT_TRUE(styled.html.contains(QLatin1String("tok-type\">int")));

    // A theme that names none: today's page, with no token classes at all.
    const ContentResult plain = render(Theme::builtin(Theme::Mode::Dark));
    EXPECT_FALSE(plain.codeStyle.contains(QLatin1String("tok-")));
    EXPECT_FALSE(plain.codeStyle.contains(QLatin1String("--code-")));
    EXPECT_FALSE(plain.html.contains(QLatin1String("tok-")));
}
