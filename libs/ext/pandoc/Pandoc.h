#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC::pandoc {

/// One thing a note can be exported as. `writer` is pandoc's own name for
/// it (`pandoc --list-output-formats`), the rest is what is passed beside it.
struct Format {
    QString id;        ///< stable, for command ids: "docx"
    QString label;     ///< for menus: "Word (.docx)"
    QString writer;    ///< "docx", "epub3", "html5"
    QString extension; ///< the output file's, without the dot
    QStringList extra; ///< arguments this format always takes
    bool referenceDoc = false; ///< takes the settings' reference.docx
};

/// DOCX, ODT, EPUB, HTML, LaTeX, RTF, PPTX, Beamer: the ones with a command of their own.
const QList<Format> &exportFormats();

/// Whether `name` can go after `-t`: a pandoc format with optional
/// extensions (`rst`, `markdown+smart-raw_html`). It never starts with a
/// dash, so it cannot be read as an option, and holds no space.
bool validFormatName(const QString &name);

/// The file extension for a pandoc output format named by hand ("Other..."):
/// `rst` for rst, `adoc` for asciidoc, the name itself when none is known.
QString extensionForWriter(const QString &writer);

/// What the Markdown is read as: CommonMark with pandoc's extensions (divs,
/// spans, footnotes, `$` maths, tables, tasks), not its own Markdown, whose
/// rules about blank lines before lists differ from what a note is written
/// against. No smart punctuation: the author's quotes stay as typed.
QString inputFormat();

/// What an import writes: GitHub Markdown, with maths as `$..$` rather than
/// GitHub's backticked `$`..`$`, which a note does not render.
QString importOutputFormat();

struct ExportRequest {
    QString writer;                 ///< -t
    QStringList formatArguments;    ///< Format::extra
    QString outputPath;             ///< absolute
    QStringList resourcePaths;      ///< where relative images are looked for
    QString referenceDoc;           ///< only when the format takes one and the setting has one
    QString title;                  ///< only when the note has none of its own
    QStringList extraArguments;     ///< the setting's, last so they win
};
/// The note is read from standard input. The list is what QProcess is given
/// as is: nothing here is ever joined into a shell line.
QStringList exportArguments(const ExportRequest &request);

struct ImportRequest {
    QString reader;                 ///< -f, from readerForSuffix()
    QString inputPath;              ///< absolute
    QString outputName;             ///< relative to the working directory
    QStringList extraArguments;
};
/// To run with the working directory a scratch folder: the media lands in
/// its `media/` folder and the Markdown names it relative to there.
QStringList importArguments(const ImportRequest &request);

/// The pandoc reader for a file's suffix (lower case, no dot), empty when
/// pandoc is not offered for it here.
QString readerForSuffix(const QString &suffix);
QStringList importSuffixes();

/// `args` split the way a shell would, without one: quotes group, nothing expands.
QStringList splitArguments(const QString &text);

/// The pandoc to run: `setting` when it is a path or a name on PATH, else
/// `pandoc` on PATH. Empty when there is none. A few stat calls, made only
/// when a pandoc command is used or the settings are opened, never at start-up.
QString locate(const QString &setting);

/// The file `media/<name>` pandoc extracted, as an Obsidian embed.
/// `names` maps a path as the Markdown writes it, relative to the scratch
/// folder ("media/rId9.png"), to the name the file was given in the vault.
/// Image tags and Markdown images naming one become `![[name]]`; a
/// `<span class="mark">` (pandoc's highlight) becomes `==text==`, and an escaped `\[ \]` task box a task's.
QString rewriteImported(const QString &markdown, const QHash<QString, QString> &names);

/// Copies every file under `<extractedRoot>/media` into `folder`, never over
/// one already there, and returns old relative path -> new file name.
QHash<QString, QString> moveMedia(const QString &extractedRoot, const QString &folder, QString *error = nullptr);

} // namespace noteAppC::pandoc
