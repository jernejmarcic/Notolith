#include "Pandoc.h"

#include "Vault.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace noteAppC::pandoc {

const QList<Format> &exportFormats()
{
    static const QList<Format> formats = {
        {QStringLiteral("docx"), QStringLiteral("Word (.docx)"), QStringLiteral("docx"), QStringLiteral("docx"), {}, true},
        {QStringLiteral("odt"), QStringLiteral("OpenDocument text (.odt)"), QStringLiteral("odt"), QStringLiteral("odt"), {}, false},
        {QStringLiteral("epub"), QStringLiteral("EPUB book (.epub)"), QStringLiteral("epub3"), QStringLiteral("epub"), {}, false},
        // Self-contained: the images are inside the page, not beside it. Pandoc fetches an image the
        // note itself takes from the web for this; a local one is read from the disk.
        {QStringLiteral("html"), QStringLiteral("HTML page (.html)"), QStringLiteral("html5"), QStringLiteral("html"),
         {QStringLiteral("--standalone"), QStringLiteral("--embed-resources")}, false},
        {QStringLiteral("latex"), QStringLiteral("LaTeX (.tex)"), QStringLiteral("latex"), QStringLiteral("tex"),
         {QStringLiteral("--standalone")}, false},
        {QStringLiteral("rtf"), QStringLiteral("Rich text (.rtf)"), QStringLiteral("rtf"), QStringLiteral("rtf"),
         {QStringLiteral("--standalone")}, false},
        {QStringLiteral("pptx"), QStringLiteral("PowerPoint (.pptx)"), QStringLiteral("pptx"), QStringLiteral("pptx"), {}, false},
        {QStringLiteral("beamer"), QStringLiteral("Beamer slides (.tex)"), QStringLiteral("beamer"), QStringLiteral("tex"),
         {QStringLiteral("--standalone")}, false},
    };
    return formats;
}

bool validFormatName(const QString &name)
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_+\\-]*$"));
    return valid.match(name).hasMatch();
}

QString defaultExportName(const QString &noteBaseName, const QString &writer, const QString &extension)
{
    if (extension.compare(QLatin1String("md"), Qt::CaseInsensitive) != 0)
        return noteBaseName + QLatin1Char('.') + extension;
    return QStringLiteral("%1 (%2).md").arg(noteBaseName, writer.section(QRegularExpression(QStringLiteral("[+\\-]")), 0, 0));
}

QString extensionForWriter(const QString &writer)
{
    static const QHash<QString, QString> known = {
        {QStringLiteral("markdown"), QStringLiteral("md")},       {QStringLiteral("gfm"), QStringLiteral("md")},
        {QStringLiteral("commonmark"), QStringLiteral("md")},     {QStringLiteral("commonmark_x"), QStringLiteral("md")},
        {QStringLiteral("markdown_strict"), QStringLiteral("md")}, {QStringLiteral("markdown_mmd"), QStringLiteral("md")},
        {QStringLiteral("markdown_phpextra"), QStringLiteral("md")}, {QStringLiteral("plain"), QStringLiteral("txt")},
        {QStringLiteral("asciidoc"), QStringLiteral("adoc")},     {QStringLiteral("asciidoctor"), QStringLiteral("adoc")},
        {QStringLiteral("mediawiki"), QStringLiteral("wiki")},    {QStringLiteral("dokuwiki"), QStringLiteral("txt")},
        {QStringLiteral("jira"), QStringLiteral("txt")},          {QStringLiteral("man"), QStringLiteral("1")},
        {QStringLiteral("docbook"), QStringLiteral("xml")},       {QStringLiteral("docbook4"), QStringLiteral("xml")},
        {QStringLiteral("docbook5"), QStringLiteral("xml")},      {QStringLiteral("jats"), QStringLiteral("xml")},
        {QStringLiteral("tei"), QStringLiteral("xml")},           {QStringLiteral("opendocument"), QStringLiteral("xml")},
        {QStringLiteral("html"), QStringLiteral("html")},         {QStringLiteral("html4"), QStringLiteral("html")},
        {QStringLiteral("html5"), QStringLiteral("html")},        {QStringLiteral("revealjs"), QStringLiteral("html")},
        {QStringLiteral("slidy"), QStringLiteral("html")},        {QStringLiteral("slideous"), QStringLiteral("html")},
        {QStringLiteral("s5"), QStringLiteral("html")},           {QStringLiteral("dzslides"), QStringLiteral("html")},
        {QStringLiteral("epub"), QStringLiteral("epub")},         {QStringLiteral("epub2"), QStringLiteral("epub")},
        {QStringLiteral("epub3"), QStringLiteral("epub")},        {QStringLiteral("latex"), QStringLiteral("tex")},
        {QStringLiteral("beamer"), QStringLiteral("tex")},        {QStringLiteral("context"), QStringLiteral("tex")},
        {QStringLiteral("texinfo"), QStringLiteral("texi")},      {QStringLiteral("typst"), QStringLiteral("typ")},
        {QStringLiteral("native"), QStringLiteral("hs")},         {QStringLiteral("bibtex"), QStringLiteral("bib")},
        {QStringLiteral("biblatex"), QStringLiteral("bib")},      {QStringLiteral("csljson"), QStringLiteral("json")},
        {QStringLiteral("ms"), QStringLiteral("ms")},
    };
    const QString base = writer.section(QRegularExpression(QStringLiteral("[+\\-]")), 0, 0).toLower();
    return known.value(base, base);
}

QString inputFormat()
{
    return QStringLiteral("commonmark_x-smart");
}

QString importOutputFormat()
{
    return QStringLiteral("gfm-tex_math_gfm+tex_math_dollars");
}

QStringList exportArguments(const ExportRequest &request)
{
    QStringList args = {QStringLiteral("-f"), inputFormat(), QStringLiteral("-t"), request.writer};
    args += request.formatArguments;
    // One flag per folder: a folder with a ':' (the list separator) in its name stays whole.
    for (const QString &folder : request.resourcePaths)
        args << QStringLiteral("--resource-path=") + folder;
    if (!request.referenceDoc.isEmpty())
        args << QStringLiteral("--reference-doc=") + request.referenceDoc;
    if (!request.title.isEmpty())
        args << QStringLiteral("--metadata=title=") + request.title;
    args += request.extraArguments;
    args << QStringLiteral("-o") << request.outputPath;
    return args;
}

QStringList importArguments(const ImportRequest &request)
{
    // The sandbox, because the file may be from anyone: without it \input, `.. include::` and #+INCLUDE read any
    // file the person can, and an <img> takes any URL. Not on export, where this pandoc cannot find its own
    // docx template under it. Pictures a sandboxed reader leaves as links are copied by moveSiblingImages.
    QStringList args = {QStringLiteral("-f"), request.reader, QStringLiteral("-t"), importOutputFormat(),
                        QStringLiteral("--wrap=none"), QStringLiteral("--standalone"),
                        QStringLiteral("--extract-media=."), QStringLiteral("--sandbox")};
    args += request.extraArguments;
    args << QStringLiteral("-o") << request.outputName << request.inputPath;
    return args;
}

namespace {

const QHash<QString, QString> &readers()
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("docx"), QStringLiteral("docx")},  {QStringLiteral("odt"), QStringLiteral("odt")},
        {QStringLiteral("epub"), QStringLiteral("epub")},  {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("htm"), QStringLiteral("html")},   {QStringLiteral("rtf"), QStringLiteral("rtf")},
        {QStringLiteral("tex"), QStringLiteral("latex")},  {QStringLiteral("latex"), QStringLiteral("latex")},
        {QStringLiteral("org"), QStringLiteral("org")},    {QStringLiteral("rst"), QStringLiteral("rst")},
        {QStringLiteral("textile"), QStringLiteral("textile")},
    };
    return map;
}

} // namespace

QString readerForSuffix(const QString &suffix)
{
    return readers().value(suffix.toLower());
}

QStringList importSuffixes()
{
    QStringList suffixes = readers().keys();
    suffixes.sort();
    return suffixes;
}

QStringList splitArguments(const QString &text)
{
    return QProcess::splitCommand(text);
}

QString locate(const QString &setting)
{
    const QString wanted = setting.trimmed();
    if (wanted.contains(QLatin1Char('/'))) {
        const QFileInfo info(wanted);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    const QString name = wanted.isEmpty() ? QStringLiteral("pandoc") : wanted;
    QString found = QStandardPaths::findExecutable(name);
    if (found.isEmpty()) {
        // A desktop launcher's PATH often lacks where a person's own install went.
        found = QStandardPaths::findExecutable(
            name, {QDir::homePath() + QStringLiteral("/.local/bin"), QDir::homePath() + QStringLiteral("/.cabal/bin"),
                   QStringLiteral("/usr/local/bin")});
    }
    return found;
}

namespace {

/// `![alt](media/x.png)`, as pandoc writes an image with no size.
const QRegularExpression &imagePattern()
{
    static const QRegularExpression image(
        QStringLiteral("!\\[([^\\]]*)\\]\\(<?([^)>\\s]+)>?(?:\\s+\"[^\"]*\")?\\)"));
    return image;
}
/// `<img src="media/x.png" style="width:2in;height:1in" alt="x" />`, as it writes one with a size.
const QRegularExpression &tagPattern()
{
    static const QRegularExpression tag(QStringLiteral("<img\\s+([^>]*?)\\s*/?>"));
    return tag;
}
const QRegularExpression &srcPattern()
{
    static const QRegularExpression src(QStringLiteral("\\bsrc=\"([^\"]*)\""));
    return src;
}

QString normalisedMediaPath(QString path)
{
    path = QUrl::fromPercentEncoding(path.toUtf8());
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    return path;
}

/// "2.08333in" and the like, in CSS pixels; 0 when it is not a length.
int pixelsOf(const QString &number, const QString &unit)
{
    const double value = number.toDouble();
    double pixels = value;
    if (unit == QLatin1String("in"))
        pixels = value * 96;
    else if (unit == QLatin1String("cm"))
        pixels = value * 96 / 2.54;
    else if (unit == QLatin1String("mm"))
        pixels = value * 96 / 25.4;
    else if (unit == QLatin1String("pt"))
        pixels = value * 96 / 72;
    return int(pixels + 0.5);
}

/// What stands where a picture could not be copied: not a link to a file that is not there.
QString lostPicture()
{
    return QStringLiteral("*(a picture could not be imported)*");
}

QString embedFor(const QString &name, int width, const QString &alt)
{
    QString alias;
    if (width > 0)
        alias = QStringLiteral("|") + QString::number(width);
    else if (!alt.isEmpty() && !alt.contains(QRegularExpression(QStringLiteral("[\\[\\]|#^\\n]"))))
        alias = QStringLiteral("|") + alt;
    return QStringLiteral("![[") + name + alias + QStringLiteral("]]");
}

} // namespace

QString rewriteImported(const QString &markdown, const QHash<QString, QString> &names)
{
    QString out = markdown;
    const QRegularExpression &image = imagePattern(), &tag = tagPattern(), &src = srcPattern();

    static const QRegularExpression alt(QStringLiteral("\\balt=\"([^\"]*)\""));
    static const QRegularExpression width(QStringLiteral("width:\\s*([0-9.]+)\\s*(in|px|cm|mm|pt)"));
    static const QRegularExpression mark(QStringLiteral("<span class=\"mark\">(.*?)</span>"),
                                         QRegularExpression::DotMatchesEverythingOption);

    const auto replaceAll = [&out](const QRegularExpression &re, auto replacement) {
        QString result;
        qsizetype last = 0;
        for (auto it = re.globalMatch(out); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            const QString text = replacement(match);
            if (text.isNull())
                continue; // not ours: stays as written
            result += QStringView(out).mid(last, match.capturedStart() - last);
            result += text;
            last = match.capturedEnd();
        }
        result += QStringView(out).mid(last);
        out = result;
    };

    replaceAll(image, [&names](const QRegularExpressionMatch &match) {
        const auto name = names.constFind(normalisedMediaPath(match.captured(2)));
        if (name == names.constEnd())
            return QString();
        return name->isEmpty() ? lostPicture() : embedFor(*name, 0, match.captured(1));
    });
    replaceAll(tag, [&names, &src](const QRegularExpressionMatch &match) {
        const QString attributes = match.captured(1);
        const auto source = src.match(attributes);
        if (!source.hasMatch())
            return QString();
        const auto name = names.constFind(normalisedMediaPath(source.captured(1)));
        if (name == names.constEnd())
            return QString();
        if (name->isEmpty())
            return lostPicture();
        const auto size = width.match(attributes);
        return embedFor(*name, size.hasMatch() ? pixelsOf(size.captured(1), size.captured(2)) : 0,
                        alt.match(attributes).captured(1));
    });
    replaceAll(mark, [](const QRegularExpressionMatch &match) {
        return QStringLiteral("==") + match.captured(1) + QStringLiteral("==");
    });
    // The empty anchors Word, ODT and EPUB headings come with (`# <span id="anchor"></span>Title`).
    static const QRegularExpression anchor(QStringLiteral("<span id=\"[^\"]*\"></span>"));
    out.remove(anchor);
    // A task list item pandoc read from Word as text, which its Markdown writer escapes.
    static const QRegularExpression task(QStringLiteral("^(\\s*(?:[-*+]|\\d+[.)])\\s+)\\\\\\[([ xX])\\\\\\]"),
                                         QRegularExpression::MultilineOption);
    out.replace(task, QStringLiteral("\\1[\\2]"));
    return out;
}

QHash<QString, QString> moveMedia(const QString &extractedRoot, const QString &folder, QString *error)
{
    QHash<QString, QString> names;
    const QDir root(extractedRoot);
    // Every file, not `media/` alone: ODT writes `Pictures/`, HTML puts them at the root under hashed names.
    for (QDirIterator it(extractedRoot, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString source = it.next();
        const QString relative = root.relativeFilePath(source);
        if (relative == QLatin1String("out.md"))
            continue;
        QDir().mkpath(folder);
        const QString target = Vault::uniqueAttachmentPath(folder, QFileInfo(source).fileName());
        if (!QFile::copy(source, target)) {
            if (error)
                *error = QStringLiteral("could not copy %1 into %2").arg(QFileInfo(source).fileName(), folder);
            names.insert(relative, QString()); // the picture is lost, and the note must not name it
            continue;
        }
        names.insert(relative, QFileInfo(target).fileName());
    }
    return names;
}

QHash<QString, QString> moveSiblingImages(const QString &markdown, const QString &inputFolder, const QString &folder,
                                          QString *error)
{
    QHash<QString, QString> names;
    const QString base = QDir(inputFolder).canonicalPath();
    if (base.isEmpty())
        return names;
    static const QStringList pictures = {QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                         QStringLiteral("gif"),  QStringLiteral("svg"),  QStringLiteral("webp"),
                                         QStringLiteral("bmp"),  QStringLiteral("avif")};
    QStringList written;
    for (auto it = imagePattern().globalMatch(markdown); it.hasNext();)
        written << it.next().captured(2);
    for (auto it = tagPattern().globalMatch(markdown); it.hasNext();) {
        const auto source = srcPattern().match(it.next().captured(1));
        if (source.hasMatch())
            written << source.captured(1);
    }
    for (const QString &raw : std::as_const(written)) {
        const QString path = normalisedMediaPath(raw);
        // Relative to the document and nothing else: no URL, no absolute path, nothing that climbs out.
        static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || scheme.match(path).hasMatch() || names.contains(path))
            continue;
        const QFileInfo file(QDir(base).filePath(path));
        const QString real = file.canonicalFilePath();
        if (real.isEmpty() || !real.startsWith(base + QLatin1Char('/')) || !pictures.contains(file.suffix().toLower()))
            continue;
        QDir().mkpath(folder);
        const QString target = Vault::uniqueAttachmentPath(folder, file.fileName());
        if (!QFile::copy(real, target)) {
            if (error)
                *error = QStringLiteral("could not copy %1 into %2").arg(file.fileName(), folder);
            names.insert(path, QString());
            continue;
        }
        names.insert(path, QFileInfo(target).fileName());
    }
    return names;
}

} // namespace noteAppC::pandoc
