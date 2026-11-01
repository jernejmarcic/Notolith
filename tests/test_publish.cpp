// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Tests for libs/ext/publish/: privacy (the first correctness property, per
// the task), link/anchor resolution, escaping, maths, attachments and the
// incremental republish. Each test builds its own throwaway vault under a
// QTemporaryDir - real files and a real Index, never a synthetic Decorations
// - and reads Publisher's own output back off disk, the way a reader's
// browser would.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtConcurrent>

#include <unistd.h>

#include "CodeHighlighter.h"
#include "Index.h"
#include "Indexer.h"
#include "MathRenderer.h"
#include "Markmap.h"
#include "MermaidCache.h"
#include "PageBody.h"
#include "Parser.h"
#include "Publisher.h"
#include "SafeHtml.h"
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

    /// `subfolder` puts the vault in a folder of vaultDir, for a test that publishes into the folder holding it.
    explicit TestSite(const QHash<QString, QByteArray> &files, const QString &subfolder = QString())
    {
        const QString base = subfolder.isEmpty() ? vaultDir.path() : vaultDir.filePath(subfolder);
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QString path = base + QLatin1Char('/') + it.key();
            QDir().mkpath(QFileInfo(path).path());
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(it.value());
        }
        vault = Vault(base);
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

TEST(PublishTest, CustomCheckboxStatesCarryDataTaskAndAClass)
{
    TestSite site({{QStringLiteral("Chores.md"), "- [-] pay rent\n- [x] call mum\n- [ ] sweep\n"}});
    const QString today = QStringLiteral("<li class=\"task-list-item done\"><input type=\"checkbox\" disabled checked> ");

    Options on;
    on.parse.taskStates = true;
    site.publish(on);
    const QString page = site.read(QStringLiteral("chores.html"));
    EXPECT_TRUE(page.contains(QLatin1String("<li class=\"task-list-item cancelled\" data-task=\"-\">"))) << page.toStdString();
    EXPECT_TRUE(page.contains(QLatin1String("<span class=\"task-box\" data-task=\"-\"></span> ")));
    EXPECT_TRUE(page.contains(QLatin1String("<span class=\"task-text strike mute\">pay rent</span>")));
    EXPECT_TRUE(page.contains(QLatin1String(".task-box[data-task=\"-\"]")));
    EXPECT_TRUE(page.contains(today)) << "[x] is exactly what it was";
    EXPECT_TRUE(page.contains(QLatin1String("<li class=\"task-list-item\"><input type=\"checkbox\" disabled> ")));

    // Off, `[-]` is text in a plain item and the page has no box CSS.
    site.publish();
    const QString plain = site.read(QStringLiteral("chores.html"));
    EXPECT_TRUE(plain.contains(QLatin1String("[-] pay rent")));
    EXPECT_FALSE(plain.contains(QLatin1String("task-box")));
    EXPECT_FALSE(plain.contains(QLatin1String("data-task")));
    EXPECT_TRUE(plain.contains(today));
}

TEST(PublishTest, CodeStylerParametersTitleNumberAndColourTheLines)
{
    TestSite site({{QStringLiteral("Code.md"), "```cpp title:\"main.cpp\" ln:5 info:2 warn:'int c'\nint a;\nint b;\nint c;\n```\n"}});
    Options on;
    on.parse.codeStyler = true;
    on.parse.highlightKinds = {QStringLiteral("info"), QStringLiteral("warn")};
    site.publish(on);
    const QString page = site.read(QStringLiteral("code.html"));
    EXPECT_TRUE(page.contains(QLatin1String("<div class=\"code-header\">main.cpp</div>"))) << page.toStdString();
    EXPECT_TRUE(page.contains(QLatin1String("code-block numbered")));
    EXPECT_TRUE(page.contains(QLatin1String("class=\"line hl hl-info\"")));
    EXPECT_TRUE(page.contains(QLatin1String("class=\"line hl hl-warn\"")));
    EXPECT_TRUE(site.read(QStringLiteral("site.css")).contains(QLatin1String(".hl-info")));

    site.publish(); // off: the line is the language and attributes it always was
    const QString plain = site.read(QStringLiteral("code.html"));
    EXPECT_FALSE(plain.contains(QLatin1String("code-header")));
    EXPECT_FALSE(plain.contains(QLatin1String("class=\"line hl")));
}

TEST(PublishTest, CodeStylerHighlightNumbersCountTheBlocksOwnLinesWhateverLnSays)
{
    TestSite site({{QStringLiteral("Code.md"), "```cpp ln:27 hl:2\nint a;\nint b;\n```\n"}});
    Options on;
    on.parse.codeStyler = true;
    site.publish(on);
    const QString page = site.read(QStringLiteral("code.html"));
    EXPECT_TRUE(page.contains(QLatin1String("class=\"line hl\" data-line=\"28\""))) << page.toStdString();
    EXPECT_FALSE(page.contains(QLatin1String("class=\"line hl\" data-line=\"27\"")));
}

TEST(PublishTest, CodeStylerInlineCodeIsHighlightedWithoutItsPrefix)
{
    TestSite site({{QStringLiteral("Inline.md"), "Say `{python} import os` and `plain`.\n"}});
    Options on;
    on.parse.codeStyler = true;
    site.publish(on);
    const QString page = site.read(QStringLiteral("inline.html"));
    EXPECT_TRUE(page.contains(QLatin1String("<code class=\"language-python\">"))) << page.toStdString();
    EXPECT_FALSE(page.contains(QLatin1String("{python}")));
    EXPECT_TRUE(page.contains(QLatin1String("<code>plain</code>")));
    site.publish();
    EXPECT_TRUE(site.read(QStringLiteral("inline.html")).contains(QLatin1String("<code>{python} import os</code>")));
}

TEST(PublishTest, SiteCSSCarriesFormattingColoursOnlyWhereSet)
{
    TestSite site({{QStringLiteral("Note.md"), "# T\n\nSome **bold** text.\n"}});
    site.publish();
    const QString plain = site.read(QStringLiteral("site.css"));
    EXPECT_TRUE(plain.contains(QLatin1String("strong { color: var(--bold-color, inherit)"))) << "the rule is always there";
    EXPECT_FALSE(plain.contains(QLatin1String("--bold-color:"))) << "unset, nothing declares it, so inherit holds";

    Options options;
    options.codeThemeDark = Theme::builtin(Theme::Mode::Dark);
    options.codeThemeDark->setToken(QStringLiteral("--bold-color"), QStringLiteral("#ff0000"));
    options.codeThemeLight = Theme::builtin(Theme::Mode::Light);
    options.codeThemeLight->setToken(QStringLiteral("--bold-color"), QStringLiteral("#00ff00"));
    site.publish(options);
    const QString css = site.read(QStringLiteral("site.css"));
    EXPECT_TRUE(css.contains(QLatin1String("--bold-color:#ff0000"))) << css.toStdString();
    EXPECT_TRUE(css.contains(QLatin1String("--bold-color:#00ff00"))) << "the light mode's own";
    EXPECT_FALSE(css.contains(QLatin1String("--h1-color:")));
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
// Privacy: the property this whole module exists for.

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

// A ```markmap fence is published as the mind map drawn from its text right there - no editor has to have drawn it
// first, so a vault published in one go is all maps - and as the plain listing when the plugin is off.
TEST(PublishTest, AMindMapIsDrawnFromTheFencesTextEvenInANoteNeverOpened)
{
    QTemporaryDir env;
    ASSERT_TRUE(env.isValid());
    const QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", env.filePath(QStringLiteral("cache")).toUtf8()); // empty: nothing was drawn before
    TestSite site({{QStringLiteral("Note.md"), "```markmap\n# Root\n## Branch\n- leaf\n```\n"}});
    site.publish();
    const QString page = site.read(QStringLiteral("note.html"));
    const QStringList images = diagramImages(page);
    ASSERT_EQ(images.size(), 1) << page.toStdString();
    EXPECT_TRUE(images.first().contains(QStringLiteral(">leaf</text>")));
    EXPECT_TRUE(images.first().contains(Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary")).name()))
        << "in the site's dark theme, like the other diagrams";

    TestSite off({{QStringLiteral("Note.md"), "```markmap\n# Root\n## Branch\n- leaf\n```\n"}});
    Options options;
    options.mindMaps = false;
    off.publish(options);
    const QString plain = off.read(QStringLiteral("note.html"));
    EXPECT_TRUE(diagramImages(plain).isEmpty());
    EXPECT_TRUE(plain.contains(QStringLiteral("# Root"))) << "the plugin is off: the listing";
    if (oldCache.isNull())
        qunsetenv("XDG_CACHE_HOME");
    else
        qputenv("XDG_CACHE_HOME", oldCache);
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
    // the *index's* cached properties, so a rebuilt cache is what
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

TEST(SafeHtmlTest, OnlyWebMailAndRelativeURLsPass)
{
    for (const char *ok : {"https://a.example/x", "HTTP://a.example", "mailto:a@b.c", "page.html#top", "../up.html",
                           "#frag", "//host/x", "dir/a:b"})
        EXPECT_TRUE(SafeHtml::isSafeURL(QString::fromLatin1(ok))) << ok;
    for (const char *bad : {"javascript:alert(1)", "JaVaScRiPt:x", " \tjava\nscript:x", "\x01javascript:x", "data:text/html,x",
                            "vbscript:x", "file:///etc/passwd", "foo:bar", "blob:x", "about:blank"})
        EXPECT_FALSE(SafeHtml::isSafeURL(QString::fromLatin1(bad))) << bad;
}

TEST(SafeHtmlTest, InlineRawHtmlIsOneTagAtATime)
{
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("<b>"), true), QStringLiteral("<b>"));
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("</b>"), true), QStringLiteral("</b>"));
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("<br>"), true), QStringLiteral("<br>"));
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("<script>"), true), QString());
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("</script>"), true), QString());
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("<a href=\"javascript:x\" onclick=\"y\" class=c>"), true),
              QStringLiteral("<a class=\"c\">"));
    EXPECT_EQ(SafeHtml::sanitize(QStringLiteral("<a href=\"https://x.example\">"), true),
              QStringLiteral("<a href=\"https://x.example\" rel=\"noopener noreferrer\">"));
}

// ---------------------------------------------------------------------------
// A vault is untrusted input: nothing it says may delete, overwrite or leak a
// file outside the site, or run as script on a published page.

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool publishInto(const TestSite &site, const QString &folder, QString *error, Counts *counts = nullptr)
{
    Publisher publisher(site.vault, site.index);
    Options options;
    options.outputDir = folder;
    const bool ok = publisher.publish(options, nullptr, error);
    if (counts)
        *counts = publisher.counts();
    return ok;
}

} // namespace

TEST(PublishSafetyTest, AHostileManifestDeletesNothingOutsideTheSite)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    QTemporaryDir elsewhere;
    const QString siteDir = site.outDir.filePath(QStringLiteral("site"));
    const QString victim = site.outDir.filePath(QStringLiteral("victim.txt"));
    const QString absoluteVictim = elsewhere.filePath(QStringLiteral("absolute.txt"));
    const QString linked = elsewhere.filePath(QStringLiteral("linked.txt"));
    writeFile(victim, "keep");
    writeFile(absoluteVictim, "keep");
    writeFile(linked, "keep");
    writeFile(siteDir + QStringLiteral("/placeholder"), "x");
    ASSERT_TRUE(QFile::link(elsewhere.path(), siteDir + QStringLiteral("/out")));

    QJsonObject manifest;
    manifest[QStringLiteral("../victim.txt")] = QStringLiteral("0");
    manifest[QStringLiteral("a/../../victim.txt")] = QStringLiteral("0");
    manifest[absoluteVictim] = QStringLiteral("0");
    manifest[QStringLiteral("out/linked.txt")] = QStringLiteral("0"); // a folder link out of the site
    manifest[QStringLiteral("a//b.html")] = QStringLiteral("0");
    manifest[QStringLiteral("a\\b.html")] = QStringLiteral("0");
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    writeFile(siteDir + QStringLiteral("/notolith-publish-manifest.json"), bytes);
    writeFile(siteDir + QStringLiteral("/noteappc-publish-manifest.json"), bytes);

    QString error;
    ASSERT_TRUE(publishInto(site, siteDir, &error)) << error.toStdString();
    EXPECT_EQ(readFile(victim), QByteArray("keep"));
    EXPECT_EQ(readFile(absoluteVictim), QByteArray("keep"));
    EXPECT_EQ(readFile(linked), QByteArray("keep"));
}

TEST(PublishSafetyTest, TheManifestHasTheNewNameAndTheOldOneIsReadOnce)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    const QString siteDir = site.outDir.path();
    writeFile(siteDir + QStringLiteral("/stale.html"), "old page");
    QJsonObject legacy;
    // The manifest records the hash of what was written; only such a file is removed as stale.
    legacy[QStringLiteral("stale.html")] =
        QString::fromLatin1(QCryptographicHash::hash("old page", QCryptographicHash::Sha1).toHex());
    writeFile(siteDir + QStringLiteral("/noteappc-publish-manifest.json"),
              QJsonDocument(legacy).toJson(QJsonDocument::Compact));

    QString error;
    ASSERT_TRUE(publishInto(site, siteDir, &error)) << error.toStdString();
    EXPECT_TRUE(site.exists(QStringLiteral("notolith-publish-manifest.json")));
    EXPECT_FALSE(site.exists(QStringLiteral("stale.html"))) << "the old manifest is still read";

    // Once the new one is there the old one is not looked at again.
    writeFile(siteDir + QStringLiteral("/stale2.html"), "kept");
    QJsonObject again;
    again[QStringLiteral("stale2.html")] = QStringLiteral("0");
    writeFile(siteDir + QStringLiteral("/noteappc-publish-manifest.json"),
              QJsonDocument(again).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(publishInto(site, siteDir, &error)) << error.toStdString();
    EXPECT_TRUE(site.exists(QStringLiteral("stale2.html")));
}

TEST(PublishSafetyTest, AnOutputSymlinkIsReplacedNeverFollowed)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    QTemporaryDir elsewhere;
    const QString victim = elsewhere.filePath(QStringLiteral("victim.txt"));
    writeFile(victim, "keep");
    ASSERT_TRUE(QFile::link(victim, site.outDir.filePath(QStringLiteral("index.html"))));
    ASSERT_TRUE(QFile::link(victim, site.outDir.filePath(QStringLiteral("site.css"))));

    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    EXPECT_EQ(readFile(victim), QByteArray("keep"));
    EXPECT_FALSE(QFileInfo(site.outDir.filePath(QStringLiteral("index.html"))).isSymLink());
    EXPECT_TRUE(site.read(QStringLiteral("index.html")).contains(QStringLiteral("<html")));
}

TEST(PublishSafetyTest, AFolderSymlinkInTheSiteFailsThePublishAndWritesNothingThrough)
{
    TestSite site({{QStringLiteral("Folder/Note.md"), "Body.\n"}});
    QTemporaryDir elsewhere;
    ASSERT_TRUE(QFile::link(elsewhere.path(), site.outDir.filePath(QStringLiteral("folder"))));

    QString error;
    EXPECT_FALSE(publishInto(site, site.outDir.path(), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("folder"))) << error.toStdString();
    EXPECT_TRUE(QDir(elsewhere.path()).isEmpty());
}

TEST(PublishSafetyTest, AFailedWriteFailsThePublishNamesTheFileAndIsNotRecorded)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    // index.html is a folder: nothing can be renamed over it.
    ASSERT_TRUE(QDir().mkpath(site.outDir.filePath(QStringLiteral("index.html/inside"))));

    QString error;
    EXPECT_FALSE(publishInto(site, site.outDir.path(), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("index.html"))) << error.toStdString();
    EXPECT_TRUE(site.exists(QStringLiteral("note.html"))); // the rest is still written
    const QByteArray manifest = readFile(site.outDir.filePath(QStringLiteral("notolith-publish-manifest.json")));
    EXPECT_FALSE(manifest.contains("index.html")) << manifest.toStdString();
    EXPECT_TRUE(manifest.contains("note.html"));
}

TEST(PublishSafetyTest, ASymlinkedAttachmentIsNotCopiedIntoTheSite)
{
    TestSite site({
        {QStringLiteral("Note.md"), "![[secret.png]] ![[real.png]]\n"},
        {QStringLiteral("real.png"), QByteArray("real-bytes")},
    });
    QTemporaryDir elsewhere;
    const QString secret = elsewhere.filePath(QStringLiteral("private.txt"));
    writeFile(secret, "PRIVATE-BYTES");
    ASSERT_TRUE(QFile::link(secret, site.vaultDir.filePath(QStringLiteral("secret.png"))));
    // The index was built before the link was there; build it again.
    Indexer indexer(&site.index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(site.vault);
    ASSERT_TRUE(done.wait(20000));

    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    EXPECT_FALSE(site.exists(QStringLiteral("attachments/secret.png")));
    EXPECT_TRUE(site.exists(QStringLiteral("attachments/real.png")));
}

TEST(PublishSafetyTest, AnAttachmentThatWasLeftOutIsListedInTheSummary)
{
    TestSite site({
        {QStringLiteral("Note.md"), "![[secret.png]]\n"},
    });
    QTemporaryDir elsewhere;
    writeFile(elsewhere.filePath(QStringLiteral("private.txt")), "PRIVATE-BYTES");
    ASSERT_TRUE(QFile::link(elsewhere.filePath(QStringLiteral("private.txt")),
                            site.vaultDir.filePath(QStringLiteral("secret.png"))));
    Indexer indexer(&site.index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(site.vault);
    ASSERT_TRUE(done.wait(20000));

    QString error;
    Counts counts;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error, &counts)) << error.toStdString();
    EXPECT_EQ(counts.skippedAttachments, QStringList{QStringLiteral("secret.png")});
}

TEST(PublishSafetyTest, NothingFromANoteRunsOnAPublishedPage)
{
    TestSite site({
        {QStringLiteral("Evil.md"),
         "#tagged\n\n"
         "[a](javascript:alert(1)) [b](JaVaScRiPt:alert(2)) [c](data:text/html,x) [d](vbscript:x) "
         "[e](file:///etc/passwd) [f](foo:bar) [[javascript:alert(3)|g]]\n\n"
         "[ok](https://example.com/x) [mail](mailto:a@example.com)\n\n"
         "<img src=x onerror=alert(1)> <img src=\"javascript:alert(2)\">\n\n"
         "<a href=\"javascript:alert(1)\">raw</a> <a href=\"  JaVa&#x73;cript:alert(2)\">raw2</a>\n\n"
         "<iframe srcdoc=\"<script>alert(1)</script>\"></iframe>\n\n"
         "<div style=\"background:url(javascript:alert(1))\" class=\"keepme\" onclick=\"x()\">\n"
         "<form action=\"https://example.com\"><input name=q></form>\n"
         "<svg onload=alert(1)><a href=\"javascript:alert(2)\"><text>t</text></a></svg>\n"
         "<object data=\"x\"></object><embed src=\"x\"><base href=\"https://evil.example/\">\n"
         "<script>alert(9)</script>\n"
         "<u>kept</u>\n</div>\n"},
    });
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();

    for (const QString &name : {QStringLiteral("evil.html"), QStringLiteral("index.html"),
                                QStringLiteral("tags/tagged.html")}) {
        const QString page = site.read(name);
        ASSERT_FALSE(page.isEmpty()) << name.toStdString();
        // Every page carries the policy and has no inline script of its own.
        EXPECT_TRUE(page.contains(QStringLiteral("http-equiv=\"Content-Security-Policy\""))) << name.toStdString();
        EXPECT_TRUE(page.contains(QStringLiteral("script-src 'self'"))) << name.toStdString();
        EXPECT_TRUE(page.contains(QStringLiteral("object-src 'none'")));
        EXPECT_TRUE(page.contains(QStringLiteral("base-uri 'none'")));
        EXPECT_TRUE(page.contains(QStringLiteral("form-action 'none'")));
        static const QRegularExpression inlineScript(QStringLiteral("<script(?![^>]*\\bsrc=)"),
                                                     QRegularExpression::CaseInsensitiveOption);
        EXPECT_FALSE(page.contains(inlineScript)) << name.toStdString();
    }
    const QString page = site.read(QStringLiteral("evil.html"));
    // Content outside the policy tag itself: no scheme that runs, no handler, no frame.
    const qsizetype body = page.indexOf(QStringLiteral("<article>"));
    ASSERT_GT(body, 0);
    const QString article = page.mid(body, page.indexOf(QStringLiteral("</article>")) - body);
    for (const char *bad : {"javascript", "vbscript", "data:text", "file:", "foo:bar", "onerror", "onclick",
                            "onload", "srcdoc", "<iframe", "<form", "<svg", "<object", "<embed", "<base",
                            "<script", "alert(9)", "url("})
        EXPECT_FALSE(article.contains(QLatin1String(bad), Qt::CaseInsensitive)) << bad << "\n" << article.toStdString();
    // The links that are fine stay, and an unsafe one keeps its text.
    EXPECT_TRUE(article.contains(QStringLiteral("href=\"https://example.com/x\"")));
    EXPECT_TRUE(article.contains(QStringLiteral("href=\"mailto:a@example.com\"")));
    EXPECT_TRUE(article.contains(QStringLiteral(">ok</a>")));
    EXPECT_TRUE(article.contains(QStringLiteral("a ")));
    EXPECT_TRUE(article.contains(QStringLiteral("class=\"keepme\"")));
    EXPECT_TRUE(article.contains(QStringLiteral("<u>kept</u>")));
}


// ---------------------------------------------------------------------------
// Round two: what a vault can still reach

namespace {

QStringList filesUnder(const QString &folder)
{
    QStringList out;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next();
    return out;
}

void reindex(TestSite &site)
{
    Indexer indexer(&site.index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(site.vault);
    ASSERT_TRUE(done.wait(20000));
}

} // namespace

TEST(PublishSafetyTest, APermalinkIsSlugifiedAndNeverReachesAPageAsMarkup)
{
    TestSite site({
        {QStringLiteral("Js.md"), "---\npermalink: 'javascript:alert(1)'\n---\n\nSee [[Evil]].\n"},
        {QStringLiteral("Evil.md"),
         "---\npermalink: 'x\"><meta http-equiv=\"refresh\" content=\"0;url=https://evil.example\"><a href=\"'\n---\n\n"
         "See [[Js]].\n"},
        {QStringLiteral("Dots.md"), "---\npermalink: ../../escape\n---\n\nBody.\n"},
        {QStringLiteral("Fine.md"), "---\npermalink: Docs/About_Us.html\n---\n\nBody [[Js]].\n"},
    });
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    const QStringList files = filesUnder(site.outDir.path());
    EXPECT_GE(files.size(), 6);
    for (const QString &file : files) {
        EXPECT_FALSE(file.contains(QLatin1Char('"')) || file.contains(QLatin1Char('<')) || file.contains(QLatin1Char(':')))
            << file.toStdString();
        const QString text = QString::fromUtf8(readFile(file));
        EXPECT_FALSE(text.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive)) << file.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("<meta http-equiv=\"refresh\""), Qt::CaseInsensitive))
            << file.toStdString();
    }
    EXPECT_TRUE(site.exists(QStringLiteral("docs/about_us.html"))) << "an ordinary permalink still works";
    EXPECT_TRUE(site.exists(QStringLiteral("dots.html"))) << "../ falls back to the default path";
}

TEST(PublishSafetyTest, AFilePlantedInTheSiteIsReplacedWhateverTheManifestSays)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    const QByteArray good = readFile(site.outDir.filePath(QStringLiteral("search.js")));
    ASSERT_FALSE(good.isEmpty());
    // The manifest is untouched; only the script is not Notolith's any more.
    writeFile(site.outDir.filePath(QStringLiteral("search.js")), "alert('planted')");
    writeFile(site.outDir.filePath(QStringLiteral("site.css")), "body{}");
    Counts counts;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error, &counts)) << error.toStdString();
    EXPECT_EQ(readFile(site.outDir.filePath(QStringLiteral("search.js"))), good);
    EXPECT_NE(readFile(site.outDir.filePath(QStringLiteral("site.css"))), QByteArray("body{}"));
}

TEST(PublishSafetyTest, OnlyAFileThisExporterWroteIsRemovedAsStale)
{
    TestSite site({
        {QStringLiteral("Stays.md"), "Body.\n"},
        {QStringLiteral("Goes.md"), "Body.\n"},
        {QStringLiteral("Edited.md"), "Body.\n"},
    });
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    // A user file, a folder and a link named in the manifest, and a page the user has since edited.
    writeFile(site.outDir.filePath(QStringLiteral("private.md")), "mine");
    ASSERT_TRUE(QDir().mkpath(site.outDir.filePath(QStringLiteral("dirkey/inside"))));
    writeFile(site.outDir.filePath(QStringLiteral("edited.html")), "my own edit");
    QJsonObject manifest = QJsonDocument::fromJson(
        readFile(site.outDir.filePath(QStringLiteral("notolith-publish-manifest.json")))).object();
    manifest[QStringLiteral("private.md")] = QStringLiteral("0000000000000000000000000000000000000000");
    manifest[QStringLiteral("dirkey")] = QStringLiteral("0000000000000000000000000000000000000000");
    writeFile(site.outDir.filePath(QStringLiteral("notolith-publish-manifest.json")),
              QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    // Goes and Edited become private.
    for (const char *name : {"Goes.md", "Edited.md"})
        writeFile(site.vaultDir.filePath(QString::fromLatin1(name)), "---\npublish: false\n---\n\nBody.\n");
    reindex(site);

    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    EXPECT_FALSE(site.exists(QStringLiteral("goes.html"))) << "its own stale page goes";
    EXPECT_EQ(readFile(site.outDir.filePath(QStringLiteral("private.md"))), QByteArray("mine"));
    EXPECT_TRUE(QDir(site.outDir.filePath(QStringLiteral("dirkey/inside"))).exists());
    EXPECT_EQ(readFile(site.outDir.filePath(QStringLiteral("edited.html"))), QByteArray("my own edit"));
    const QByteArray after = readFile(site.outDir.filePath(QStringLiteral("notolith-publish-manifest.json")));
    for (const char *key : {"private.md", "dirkey", "edited.html", "goes.html"})
        EXPECT_FALSE(after.contains(key)) << key << " is not kept in the manifest";
}

TEST(PublishSafetyTest, AStaleFileThatCannotBeRemovedFailsThePublishAndStaysTracked)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "root ignores folder permissions";
    TestSite site({
        {QStringLiteral("Stays.md"), "Body.\n"},
        {QStringLiteral("Folder/Goes.md"), "Body.\n"},
    });
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    writeFile(site.vaultDir.filePath(QStringLiteral("Folder/Goes.md")), "---\npublish: false\n---\n\nBody.\n");
    reindex(site);
    const QString folder = site.outDir.filePath(QStringLiteral("folder"));
    ASSERT_TRUE(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    EXPECT_FALSE(publishInto(site, site.outDir.path(), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("goes.html"))) << error.toStdString();
    const QByteArray manifest = readFile(site.outDir.filePath(QStringLiteral("notolith-publish-manifest.json")));
    EXPECT_TRUE(manifest.contains("folder/goes.html")) << "still tracked, so the next publish tries again";
    ASSERT_TRUE(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    EXPECT_FALSE(site.exists(QStringLiteral("folder/goes.html")));
}

TEST(PublishSafetyTest, TheOutputFolderIsNeverALinkNorTheVaultNorHoldsIt)
{
    TestSite site({{QStringLiteral("Note.md"), "Body.\n"}});
    QTemporaryDir elsewhere;
    const QString viaLink = site.outDir.filePath(QStringLiteral("link-out"));
    ASSERT_TRUE(QFile::link(elsewhere.path(), viaLink));
    QString error;
    EXPECT_FALSE(publishInto(site, viaLink, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QDir(elsewhere.path()).isEmpty());

    // A link to the vault gets past the lexical "is it the vault root" test.
    const QString toVault = site.outDir.filePath(QStringLiteral("link-vault"));
    ASSERT_TRUE(QFile::link(site.vaultDir.path(), toVault));
    EXPECT_FALSE(publishInto(site, toVault, &error));
    EXPECT_EQ(QDir(site.vaultDir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1) << "only Note.md";

    // A folder that holds the vault.
    TestSite nested({{QStringLiteral("Note.md"), "Body.\n"}}, QStringLiteral("vault"));
    EXPECT_FALSE(publishInto(nested, nested.vaultDir.path(), &error));
    EXPECT_TRUE(QDir(nested.vaultDir.path()).entryList(QDir::Files).isEmpty());
    // A link higher up the path is the person's own business.
    ASSERT_TRUE(QFile::link(site.outDir.path(), elsewhere.filePath(QStringLiteral("home"))));
    EXPECT_TRUE(publishInto(site, elsewhere.filePath(QStringLiteral("home/site")), &error)) << error.toStdString();
}

TEST(PublishSafetyTest, AnAttachmentReachedThroughALinkInsideTheVaultIsLeftOut)
{
    TestSite site({
        {QStringLiteral("Note.md"), "![[link.png]] ![[plain.png]]\n"},
        {QStringLiteral("plain.png"), QByteArray("plain-bytes")},
        {QStringLiteral("real.png"), QByteArray("real-bytes")},
    });
    // Both ends inside the vault, so resolvesInside() passes; the copy still opens without following it.
    ASSERT_TRUE(QFile::link(site.vaultDir.filePath(QStringLiteral("real.png")),
                            site.vaultDir.filePath(QStringLiteral("link.png"))));
    reindex(site);
    QString error;
    Counts counts;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error, &counts)) << error.toStdString();
    EXPECT_FALSE(site.exists(QStringLiteral("attachments/link.png")));
    EXPECT_TRUE(site.exists(QStringLiteral("attachments/plain.png")));
    EXPECT_EQ(counts.skippedAttachments, QStringList{QStringLiteral("link.png")});
}

TEST(PublishSafetyTest, ANoteThatCannotBeReadFailsThePublishAndKeepsItsOldPage)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "root reads any file";
    TestSite site({
        {QStringLiteral("Fine.md"), "Body.\n"},
        {QStringLiteral("Locked.md"), "Body.\n"},
    });
    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    ASSERT_TRUE(site.exists(QStringLiteral("locked.html")));
    ASSERT_TRUE(QFile::setPermissions(site.vaultDir.filePath(QStringLiteral("Locked.md")), QFileDevice::WriteOwner));

    EXPECT_FALSE(publishInto(site, site.outDir.path(), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("Locked.md"))) << error.toStdString();
    EXPECT_TRUE(site.exists(QStringLiteral("locked.html"))) << "its old page is not cleaned away";
    EXPECT_TRUE(site.exists(QStringLiteral("fine.html")));
    ASSERT_TRUE(QFile::setPermissions(site.vaultDir.filePath(QStringLiteral("Locked.md")),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    EXPECT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
}

TEST(PublishSafetyTest, TempFilesOfACrashedPublishAreSweptAndNothingElse)
{
    TestSite site({{QStringLiteral("Folder/Note.md"), "Body.\n"}});
    QTemporaryDir elsewhere;
    const QString hex = QStringLiteral("0123456789abcdef0123456789abcdef");
    writeFile(site.outDir.filePath(QStringLiteral(".tmp-") + hex), "half");
    writeFile(site.outDir.filePath(QStringLiteral("folder/.tmp-") + hex), "half");
    writeFile(site.outDir.filePath(QStringLiteral(".tmp-notours")), "kept");
    writeFile(site.outDir.filePath(QStringLiteral("tmp-") + hex), "kept");
    ASSERT_TRUE(QDir().mkpath(site.outDir.filePath(QStringLiteral(".tmp-1123456789abcdef0123456789abcdef/in"))));
    writeFile(elsewhere.filePath(QStringLiteral(".tmp-") + hex), "outside");
    ASSERT_TRUE(QFile::link(elsewhere.path(), site.outDir.filePath(QStringLiteral("linked"))));

    QString error;
    ASSERT_TRUE(publishInto(site, site.outDir.path(), &error)) << error.toStdString();
    EXPECT_FALSE(site.exists(QStringLiteral(".tmp-") + hex));
    EXPECT_FALSE(site.exists(QStringLiteral("folder/.tmp-") + hex));
    EXPECT_TRUE(site.exists(QStringLiteral(".tmp-notours")));
    EXPECT_TRUE(site.exists(QStringLiteral("tmp-") + hex));
    EXPECT_TRUE(QDir(site.outDir.filePath(QStringLiteral(".tmp-1123456789abcdef0123456789abcdef/in"))).exists());
    EXPECT_TRUE(QFile::exists(elsewhere.filePath(QStringLiteral(".tmp-") + hex))) << "a link to a folder is not entered";
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
