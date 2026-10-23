// Pandoc export and import: the Markdown pre-conversion and the argument lists are pure
// and always tested; the last tests run the real pandoc when there is one (and say so when
// they skip), on a small note -> docx -> import round trip.

#include "Pandoc.h"
#include "PandocPrepare.h"
#include "PandocRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace noteAppC;
using namespace noteAppC::pandoc;

// So a failure prints the text, not its UTF-16 code units.
void PrintTo(const QString &text, std::ostream *out)
{
    *out << '"' << text.toStdString() << '"';
}

namespace {

/// A vault of a few names: `files` maps what a link says to the path it resolves to.
PrepareContext contextFor(const QHash<QString, QString> &files, const QHash<QString, QString> &notes = {})
{
    PrepareContext context;
    context.resolve = [files](const QString &target, const QString &) { return files.value(target); };
    context.readNote = [notes](const QString &path) -> std::optional<QString> {
        const auto found = notes.constFind(path);
        return found == notes.constEnd() ? std::nullopt : std::optional<QString>(*found);
    };
    return context;
}

QString prepared(const QString &text, const PrepareContext &context = {})
{
    return prepareMarkdown(text, QStringLiteral("/v/dir/Note.md"), context).markdown;
}

} // namespace

TEST(PandocPrepare, PlainMarkdownIsUntouched)
{
    const QString text = QStringLiteral("---\ntags: [a, b]\n---\n# Title\n\nSome *text* with $x^2$ and `code`.\n\n$$\ny\n$$\n");
    EXPECT_EQ(prepared(text), text);
}

TEST(PandocPrepare, WikilinksBecomeLinks)
{
    const auto context = contextFor({{QStringLiteral("Other"), QStringLiteral("/v/dir/sub/Other note.md")}});
    EXPECT_EQ(prepared(QStringLiteral("See [[Other|the other]] now."), context),
              QStringLiteral("See [the other](<sub/Other note.md>) now."));
    EXPECT_EQ(prepared(QStringLiteral("[[Other#Part One]]"), context),
              QStringLiteral("[Other > Part One](<sub/Other note.md#part-one>)"));
    // Names nothing: only the text stays, and it is counted.
    const PrepareResult missing = prepareMarkdown(QStringLiteral("[[Nowhere]] and [[Nowhere|alias]]"),
                                                  QStringLiteral("/v/dir/Note.md"), contextFor({}));
    EXPECT_EQ(missing.markdown, QStringLiteral("Nowhere and alias"));
    EXPECT_EQ(missing.unresolved, 2);
    // Inside this note: the heading's id; a block link is its text.
    EXPECT_EQ(prepared(QStringLiteral("[[#My Heading!]] [[#^abc|block]]")),
              QStringLiteral("[My Heading!](#my-heading) block"));
    EXPECT_EQ(prepared(QStringLiteral("[[https://example.com/a b|site]]")),
              QStringLiteral("[site](<https://example.com/a b>)"));
}

TEST(PandocPrepare, CodeAndMathAreNotTouched)
{
    const QString text = QStringLiteral("`[[x]]` and $[[y]]$\n\n```\n[[z]] ==a== %%b%%\n```\n");
    EXPECT_EQ(prepared(text), text);
}

TEST(PandocPrepare, ImageEmbeds)
{
    const auto context = contextFor({{QStringLiteral("pic.png"), QStringLiteral("/v/att/my pic.png")}});
    EXPECT_EQ(prepared(QStringLiteral("![[pic.png]]"), context), QStringLiteral("![](</v/att/my pic.png>)"));
    EXPECT_EQ(prepared(QStringLiteral("![[pic.png|300]]"), context), QStringLiteral("![](</v/att/my pic.png>){width=300}"));
    EXPECT_EQ(prepared(QStringLiteral("![[pic.png|300x200]]"), context),
              QStringLiteral("![](</v/att/my pic.png>){width=300 height=200}"));
    EXPECT_EQ(prepared(QStringLiteral("![[pic.png|the picture]]"), context), QStringLiteral("![the picture](</v/att/my pic.png>)"));
    // A Markdown image is the author's own and stays.
    EXPECT_EQ(prepared(QStringLiteral("![x](rel/a.png)"), context), QStringLiteral("![x](rel/a.png)"));
}

TEST(PandocPrepare, NoteEmbedsAreInlined)
{
    const QString other = QStringLiteral("/v/dir/Other.md");
    const QHash<QString, QString> notes = {
        {other, QStringLiteral("---\ntitle: x\n---\nIntro\n\n## Part\nPart text [[Third]]\n\n## Next\nafter\n\nA block ^b1\n")}};
    auto context = contextFor({{QStringLiteral("Other"), other}, {QStringLiteral("Third"), QStringLiteral("/v/dir/Third.md")}}, notes);

    // Whole note, frontmatter gone, its own links resolved from its place.
    EXPECT_EQ(prepared(QStringLiteral("before\n\n![[Other]]\n\nafter"), context),
              QStringLiteral("before\n\nIntro\n\n## Part\nPart text [Third](<Third.md>)\n\n## Next\nafter\n\nA block\n\nafter"));
    EXPECT_EQ(prepared(QStringLiteral("![[Other#Part]]"), context), QStringLiteral("## Part\nPart text [Third](<Third.md>)"));
    EXPECT_EQ(prepared(QStringLiteral("![[Other#^b1]]"), context), QStringLiteral("A block"));
    // In a quote: every line keeps the quote.
    EXPECT_EQ(prepared(QStringLiteral("> ![[Other#Part]]"), context), QStringLiteral("> ## Part\n> Part text [Third](<Third.md>)"));
    // In the middle of a line, a link; a section that is not there, a link too.
    EXPECT_EQ(prepared(QStringLiteral("see ![[Other]] here"), context), QStringLiteral("see [Other](<Other.md>) here"));
    EXPECT_EQ(prepared(QStringLiteral("![[Other#Missing]]"), context), QStringLiteral("[Other](<Other.md>)"));
}

TEST(PandocPrepare, EmbedCyclesStop)
{
    const QString a = QStringLiteral("/v/dir/A.md");
    const QString b = QStringLiteral("/v/dir/B.md");
    const QHash<QString, QString> notes = {{a, QStringLiteral("A text\n\n![[B]]\n")}, {b, QStringLiteral("B text\n\n![[A]]\n")}};
    const auto context = contextFor({{QStringLiteral("A"), a}, {QStringLiteral("B"), b}}, notes);
    const PrepareResult result = prepareMarkdown(QStringLiteral("![[A]]"), QStringLiteral("/v/dir/Note.md"), context);
    EXPECT_EQ(result.markdown, QStringLiteral("A text\n\nB text\n\n[A](<A.md>)"));
    EXPECT_EQ(result.cycles, 1);
}

TEST(PandocPrepare, OtherFilesAndMissingEmbeds)
{
    const auto context = contextFor({{QStringLiteral("doc.pdf"), QStringLiteral("/v/files/doc.pdf")}});
    EXPECT_EQ(prepared(QStringLiteral("![[doc.pdf]]"), context), QStringLiteral("[doc.pdf](<../files/doc.pdf>)"));
    const PrepareResult missing = prepareMarkdown(QStringLiteral("![[gone.png]]"), QStringLiteral("/v/dir/Note.md"), contextFor({}));
    EXPECT_EQ(missing.markdown, QStringLiteral("*gone.png*"));
    EXPECT_EQ(missing.unresolved, 1);
}

TEST(PandocPrepare, Callouts)
{
    EXPECT_EQ(prepared(QStringLiteral("> [!note] A [[x|title]]\n> body\n> more\n\nafter")),
              QStringLiteral("> **A title**\n>\n> body\n> more\n\nafter"));
    EXPECT_EQ(prepared(QStringLiteral("> [!warning]-\n> body")), QStringLiteral("> **Warning**\n>\n> body"));
    EXPECT_EQ(prepared(QStringLiteral("> [!tip]")), QStringLiteral("> **Tip**"));
    EXPECT_EQ(prepared(QStringLiteral("> [!note] Outer\n> > [!tip] Inner\n> > text")),
              QStringLiteral("> **Outer**\n>\n> > **Inner**\n> >\n> > text"));
    // A `:::` container is a div pandoc knows.
    EXPECT_EQ(prepared(QStringLiteral(":::note title=\"Hi\"\nbody\n:::\n")),
              QStringLiteral("::: {.callout .callout-note}\n\n**Hi**\n\nbody\n:::\n"));
    // A plain quote is a quote.
    EXPECT_EQ(prepared(QStringLiteral("> just [quoted]")), QStringLiteral("> just [quoted]"));
}

TEST(PandocPrepare, HighlightsCommentsAndBlockIDs)
{
    EXPECT_EQ(prepared(QStringLiteral("a ==marked [[x]]== b")), QStringLiteral("a [marked x]{.mark} b"));
    EXPECT_EQ(prepared(QStringLiteral("keep %%drop%% this")), QStringLiteral("keep  this"));
    EXPECT_EQ(prepared(QStringLiteral("one\n%%\nwhole\nlines\n%%\ntwo")), QStringLiteral("one\ntwo"));
    EXPECT_EQ(prepared(QStringLiteral("A paragraph ^abc123\n\nnext")), QStringLiteral("A paragraph\n\nnext"));
}

TEST(PandocPrepare, DiagramsAreTheCachedPicture)
{
    PrepareContext context;
    QStringList asked;
    context.diagramImage = [&asked](const QString &language, const QString &source) {
        asked << language + QLatin1Char(':') + source.trimmed();
        return language == QLatin1String("mermaid") ? QStringLiteral("/cache/d.png") : QString();
    };
    EXPECT_EQ(prepared(QStringLiteral("x\n\n```mermaid\ngraph TD\nA-->B\n```\n\n```python\nprint(1)\n```\n"), context),
              QStringLiteral("x\n\n![](</cache/d.png>)\n\n```python\nprint(1)\n```\n"));
    EXPECT_EQ(asked, (QStringList{QStringLiteral("mermaid:graph TD\nA-->B"), QStringLiteral("python:print(1)")}));
}

TEST(PandocArgs, EveryFormat)
{
    for (const Format &format : exportFormats()) {
        ExportRequest request;
        request.writer = format.writer;
        request.formatArguments = format.extra;
        request.outputPath = QStringLiteral("/out/n.") + format.extension;
        const QStringList args = exportArguments(request);
        ASSERT_GE(args.size(), 6) << qPrintable(format.id);
        EXPECT_EQ(args.mid(0, 4), (QStringList{QStringLiteral("-f"), QStringLiteral("commonmark_x-smart"), QStringLiteral("-t"), format.writer}));
        EXPECT_EQ(args.mid(args.size() - 2), (QStringList{QStringLiteral("-o"), request.outputPath}));
        EXPECT_TRUE(validFormatName(format.writer));
    }
    EXPECT_TRUE(exportFormats().first().referenceDoc);
    QStringList ids;
    for (const Format &format : exportFormats())
        ids << format.id;
    EXPECT_EQ(ids, (QStringList{QStringLiteral("docx"), QStringLiteral("odt"), QStringLiteral("epub"), QStringLiteral("html"),
                                QStringLiteral("latex"), QStringLiteral("rtf"), QStringLiteral("pptx"), QStringLiteral("beamer")}));
}

TEST(PandocArgs, Details)
{
    ExportRequest request;
    request.writer = QStringLiteral("docx");
    request.outputPath = QStringLiteral("/out/n.docx");
    request.resourcePaths = {QStringLiteral("/v/dir"), QStringLiteral("/v")};
    request.referenceDoc = QStringLiteral("/ref.docx");
    request.title = QStringLiteral("My: note");
    request.extraArguments = {QStringLiteral("--toc")};
    EXPECT_EQ(exportArguments(request),
              (QStringList{QStringLiteral("-f"), QStringLiteral("commonmark_x-smart"), QStringLiteral("-t"), QStringLiteral("docx"),
                           QStringLiteral("--resource-path=/v/dir:/v"), QStringLiteral("--reference-doc=/ref.docx"),
                           QStringLiteral("--metadata=title=My: note"), QStringLiteral("--toc"), QStringLiteral("-o"),
                           QStringLiteral("/out/n.docx")}));

    ImportRequest in;
    in.reader = QStringLiteral("docx");
    in.inputPath = QStringLiteral("/in/a b.docx");
    in.outputName = QStringLiteral("out.md");
    EXPECT_EQ(importArguments(in),
              (QStringList{QStringLiteral("-f"), QStringLiteral("docx"), QStringLiteral("-t"), QStringLiteral("gfm-tex_math_gfm+tex_math_dollars"),
                           QStringLiteral("--wrap=none"), QStringLiteral("--standalone"), QStringLiteral("--extract-media=."),
                           QStringLiteral("-o"), QStringLiteral("out.md"), QStringLiteral("/in/a b.docx")}));
}

TEST(PandocArgs, FormatNamesAndSuffixes)
{
    EXPECT_TRUE(validFormatName(QStringLiteral("rst")));
    EXPECT_TRUE(validFormatName(QStringLiteral("markdown+smart-raw_html")));
    EXPECT_FALSE(validFormatName(QStringLiteral("")));
    EXPECT_FALSE(validFormatName(QStringLiteral("--lua-filter=x")));
    EXPECT_FALSE(validFormatName(QStringLiteral("rst -o /etc/x")));
    EXPECT_EQ(extensionForWriter(QStringLiteral("asciidoc")), QStringLiteral("adoc"));
    EXPECT_EQ(extensionForWriter(QStringLiteral("markdown+smart")), QStringLiteral("md"));
    EXPECT_EQ(extensionForWriter(QStringLiteral("org")), QStringLiteral("org"));
    EXPECT_EQ(readerForSuffix(QStringLiteral("DOCX")), QStringLiteral("docx"));
    EXPECT_EQ(readerForSuffix(QStringLiteral("htm")), QStringLiteral("html"));
    EXPECT_EQ(readerForSuffix(QStringLiteral("tex")), QStringLiteral("latex"));
    EXPECT_EQ(readerForSuffix(QStringLiteral("md")), QString());
    EXPECT_EQ(splitArguments(QStringLiteral("--toc --metadata=\"a b=c d\" -V \"x y\"")),
              (QStringList{QStringLiteral("--toc"), QStringLiteral("--metadata=a b=c d"), QStringLiteral("-V"), QStringLiteral("x y")}));
}

TEST(PandocImport, RewritesWhatNotolithReads)
{
    const QHash<QString, QString> names = {{QStringLiteral("media/rId9.png"), QStringLiteral("rId9.png")},
                                           {QStringLiteral("media/a b.png"), QStringLiteral("a b 1.png")}};
    EXPECT_EQ(rewriteImported(QStringLiteral("![cap](media/rId9.png) ![](./media/a%20b.png) ![w](media/other.png)"), names),
              QStringLiteral("![[rId9.png|cap]] ![[a b 1.png]] ![w](media/other.png)"));
    EXPECT_EQ(rewriteImported(QStringLiteral("<img src=\"./media/rId9.png\" style=\"width:2in;height:1in\" alt=\"x\" />"), names),
              QStringLiteral("![[rId9.png|192]]"));
    EXPECT_EQ(rewriteImported(QStringLiteral("a <span class=\"mark\">hi *there*</span> b"), names),
              QStringLiteral("a ==hi *there*== b"));
    EXPECT_EQ(rewriteImported(QStringLiteral("- \\[ \\] a\n  - \\[x\\] b\n\\[ \\] not a list"), names),
              QStringLiteral("- [ ] a\n  - [x] b\n\\[ \\] not a list"));
}

TEST(PandocImport, MediaIsCopiedNeverOver)
{
    QTemporaryDir scratch;
    QTemporaryDir vault;
    ASSERT_TRUE(QDir(scratch.path()).mkpath(QStringLiteral("media/sub")));
    for (const char *name : {"media/one.png", "media/sub/two.png"}) {
        QFile file(scratch.filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("new");
    }
    QFile existing(vault.filePath(QStringLiteral("one.png")));
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    existing.write("keep");
    existing.close();

    const QHash<QString, QString> names = moveMedia(scratch.path(), vault.path());
    EXPECT_EQ(names.value(QStringLiteral("media/one.png")), QStringLiteral("one 1.png"));
    EXPECT_EQ(names.value(QStringLiteral("media/sub/two.png")), QStringLiteral("two.png"));
    QFile kept(vault.filePath(QStringLiteral("one.png")));
    ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
    EXPECT_EQ(kept.readAll(), QByteArray("keep"));
}

// ---- the program itself -------------------------------------------------

TEST(PandocLocate, MissingPandocIsEmpty)
{
    EXPECT_EQ(locate(QStringLiteral("/nonexistent/dir/pandoc")), QString());
    EXPECT_EQ(locate(QStringLiteral("pandoc-that-is-not-installed-anywhere")), QString());
    QTemporaryDir dir;
    // A file that is not executable is not a pandoc.
    QFile plain(dir.filePath(QStringLiteral("pandoc")));
    ASSERT_TRUE(plain.open(QIODevice::WriteOnly));
    plain.write("#!/bin/sh\n");
    plain.close();
    EXPECT_EQ(locate(plain.fileName()), QString());
    plain.setPermissions(plain.permissions() | QFileDevice::ExeOwner);
    EXPECT_EQ(locate(plain.fileName()), plain.fileName());
}

TEST(PandocRunner, ReportsAFailureWithItsMessage)
{
    QTemporaryDir dir;
    Runner runner;
    QEventLoop loop;
    bool ok = true;
    QString message;
    QObject::connect(&runner, &Runner::finished, &loop, [&](bool success, const QString &text) {
        ok = success;
        message = text;
        loop.quit();
    });
    runner.start(QStringLiteral("/nonexistent/dir/pandoc"), {QStringLiteral("-v")}, {}, dir.path());
    if (runner.running())
        loop.exec();
    EXPECT_FALSE(ok);
    EXPECT_FALSE(message.isEmpty());
}

TEST(PandocRunner, CancelAndTimeout)
{
    QTemporaryDir dir;
    for (const bool cancel : {true, false}) {
        Runner runner;
        QEventLoop loop;
        bool ok = true;
        QString message;
        QObject::connect(&runner, &Runner::finished, &loop, [&](bool success, const QString &text) {
            ok = success;
            message = text;
            loop.quit();
        });
        runner.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")}, {}, dir.path(), cancel ? 120000 : 200);
        if (cancel)
            QMetaObject::invokeMethod(&runner, &Runner::cancel, Qt::QueuedConnection);
        loop.exec();
        EXPECT_FALSE(ok);
        EXPECT_EQ(message.contains(QStringLiteral("Cancelled")), cancel) << qPrintable(message);
        EXPECT_EQ(message.contains(QStringLiteral("too long")), !cancel) << qPrintable(message);
    }
}

namespace {

/// Runs `program` and waits for it; empty on success, else the message.
QString runAndWait(const QString &program, const QStringList &args, const QByteArray &input, const QString &cwd)
{
    Runner runner;
    QEventLoop loop;
    QString result = QStringLiteral("not finished");
    QObject::connect(&runner, &Runner::finished, &loop, [&](bool, const QString &message) {
        result = message;
        loop.quit();
    });
    runner.start(program, args, input, cwd);
    if (runner.running())
        loop.exec();
    return result;
}

} // namespace

TEST(PandocReal, NoteToDocxAndBackToMarkdown)
{
    const QString pandoc = locate(QString());
    if (pandoc.isEmpty())
        GTEST_SKIP() << "pandoc is not installed";

    QTemporaryDir dir;
    const QString picture = dir.filePath(QStringLiteral("my pic.png"));
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(picture));

    const QString notePath = dir.filePath(QStringLiteral("Round trip.md"));
    const QString note = QStringLiteral(
        "---\ntitle: Round trip\n---\n# Heading\n\nSome ==marked== text, a [[Other|link]], $x^2$ and %%hidden%%.\n\n"
        "> [!tip] Remember\n> the body\n\n![[my pic.png|200]]\n\n- [ ] a task\n- [x] done\n\n| a | b |\n|---|---|\n| 1 | 2 |\n");
    PrepareContext context;
    context.resolve = [&](const QString &target, const QString &) {
        return target == QLatin1String("my pic.png") ? picture : QString();
    };
    const PrepareResult prepared = prepareMarkdown(note, notePath, context);
    EXPECT_FALSE(prepared.markdown.contains(QStringLiteral("hidden")));

    ExportRequest request;
    request.writer = QStringLiteral("docx");
    request.outputPath = dir.filePath(QStringLiteral("out.docx"));
    request.resourcePaths = {dir.path()};
    ASSERT_EQ(runAndWait(pandoc, exportArguments(request), prepared.markdown.toUtf8(), dir.path()), QString());
    ASSERT_TRUE(QFileInfo(request.outputPath).size() > 1000);

    // Import it into a scratch folder, then through the same steps the window takes.
    QTemporaryDir scratch;
    ImportRequest in;
    in.reader = readerForSuffix(QStringLiteral("docx"));
    in.inputPath = request.outputPath;
    in.outputName = QStringLiteral("out.md");
    ASSERT_EQ(runAndWait(pandoc, importArguments(in), {}, scratch.path()), QString());
    QFile result(scratch.filePath(QStringLiteral("out.md")));
    ASSERT_TRUE(result.open(QIODevice::ReadOnly));
    const QString markdown = QString::fromUtf8(result.readAll());

    QTemporaryDir vault;
    const QHash<QString, QString> names = moveMedia(scratch.path(), vault.path());
    ASSERT_EQ(names.size(), 1);
    const QString final = rewriteImported(markdown, names);
    EXPECT_TRUE(final.contains(QStringLiteral("title: Round trip"))) << qPrintable(final);
    EXPECT_TRUE(final.contains(QStringLiteral("# Heading")));
    EXPECT_TRUE(final.contains(QStringLiteral("==marked==")));
    EXPECT_TRUE(final.contains(QStringLiteral("$x^{2}$")));
    EXPECT_TRUE(final.contains(QStringLiteral("**Remember**")));
    EXPECT_TRUE(final.contains(QStringLiteral("![[rId")) && final.contains(QStringLiteral(".png|")));
    EXPECT_TRUE(final.contains(QStringLiteral("- [ ] a task"))) << qPrintable(final);
    EXPECT_TRUE(final.contains(QStringLiteral("| a")));
    EXPECT_FALSE(final.contains(QStringLiteral("hidden")));
    EXPECT_TRUE(QFileInfo::exists(vault.filePath(*names.cbegin())));
}

TEST(PandocReal, AFailureShowsPandocsOwnWords)
{
    const QString pandoc = locate(QString());
    if (pandoc.isEmpty())
        GTEST_SKIP() << "pandoc is not installed";
    QTemporaryDir dir;
    const QString message = runAndWait(pandoc, {QStringLiteral("-f"), QStringLiteral("nonsense"), QStringLiteral("-o"),
                                                dir.filePath(QStringLiteral("x.md")), QStringLiteral("/nonexistent/in.docx")},
                                       {}, dir.path());
    EXPECT_FALSE(message.isEmpty());
    EXPECT_TRUE(message.contains(QStringLiteral("nonsense")) || message.contains(QStringLiteral("in.docx"))) << qPrintable(message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
