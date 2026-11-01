#include "TexCompiler.h"
#include "ConfigFile.h"

#include "Bubblewrap.h"
#include "SyscallFilter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace noteAppC {

namespace {

constexpr qint64 kMaxPDFBytes = 64LL * 1024 * 1024; ///< the biggest PDF a document may make
constexpr qint64 kMaxLogBytes = 4LL * 1024 * 1024;  ///< and the most it may print

/// `$XDG_CACHE_HOME/notolith/tex`, or `~/.cache/notolith/tex`.
QString cacheRoot()
{
    return appDir(XDGroot::Cache, QStringLiteral("tex"));
}

/// The last `maxLines` non-empty lines of pdflatex's log - enough to show
/// what went wrong without dumping the whole run into the embed.
QString tail(const QString &text, int maxLines)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.constLast().trimmed().isEmpty())
        lines.removeLast();
    if (lines.size() > maxLines)
        lines = lines.mid(lines.size() - maxLines);
    return lines.join(QLatin1Char('\n'));
}

} // namespace

bool TexCompiler::isFullDocument(const QString &texSource)
{
    return texSource.contains(QLatin1String("\\documentclass"));
}

QString TexCompiler::enginePath()
{
    return QStandardPaths::findExecutable(QStringLiteral("pdflatex"));
}

bool TexCompiler::isAvailable()
{
    return !enginePath().isEmpty();
}

TexCompiler::Result TexCompiler::compile(const QString &texPath)
{
    Result result;

    QFile source(texPath);
    if (!source.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("cannot read %1").arg(texPath);
        return result;
    }
    const QByteArray bytes = source.readAll();

    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
    const QString outDir = cacheRoot() + QLatin1Char('/') + hash;
    const QString pdfPath = outDir + QStringLiteral("/out.pdf");
    const QString failPath = outDir + QStringLiteral("/fail.log");

    // Unchanged bytes hash to the same directory, so a second embed of the
    // same content - or the same note reopened - replays the answer instead
    // of paying for another compile, success or failure alike.
    if (QFile::exists(pdfPath)) {
        result.ok = true;
        result.pdfPath = pdfPath;
        return result;
    }
    if (QFile::exists(failPath)) {
        QFile log(failPath);
        if (log.open(QIODevice::ReadOnly))
            result.log = QString::fromUtf8(log.readAll());
        return result;
    }

    const QString engine = enginePath();
    if (engine.isEmpty()) {
        result.error = QStringLiteral("no TeX engine found on PATH (install texlive)");
        return result;
    }

    QTemporaryDir work;
    if (!work.isValid()) {
        result.error = QStringLiteral("could not create a build directory");
        return result;
    }
    const QString jobName = QStringLiteral("doc");
    const QString inputPath = work.filePath(jobName + QStringLiteral(".tex"));
    QFile input(inputPath);
    if (!input.open(QIODevice::WriteOnly) || input.write(bytes) < 0) {
        result.error = QStringLiteral("could not write the compile directory");
        return result;
    }
    input.close();

    // -no-shell-escape stops \write18, -halt-on-error and nonstopmode end a document that errors, and
    // -no-parse-first-line keeps the file's own first line from setting options. None of that confines
    // \input, which reads any file the user can and sets it into the PDF, nor a TeX that never errors:
    // the sandbox does - the system and the build directory and nothing else: no home, no other drive, no
    // socket, no network - and the limits below, on the whole job. Without bubblewrap the run is refused.
    const QStringList arguments = {QStringLiteral("-no-shell-escape"), QStringLiteral("-interaction=nonstopmode"),
                                   QStringLiteral("-halt-on-error"), QStringLiteral("-no-parse-first-line"),
                                   QStringLiteral("-jobname=") + jobName, inputPath};
    const sandbox::Command wrapped = sandbox::bubblewrapCommand(work.path(), engine, arguments, {QStringLiteral("openout_any=p")});
    if (wrapped.program.isEmpty()) {
        result.error = QStringLiteral("TeX runs only inside bubblewrap's sandbox; install bubblewrap");
        return result;
    }
    sandbox::Limits limits;
    limits.cpuSeconds = 25;
    limits.addressSpace = 2ULL * 1024 * 1024 * 1024;
    limits.fileSize = kMaxPDFBytes;
    limits.openFiles = 256;
    const sandbox::Outcome ran = sandbox::run(wrapped, limits, {}, 30000, kMaxLogBytes, /*mergeChannels=*/true);
    const bool finished = ran.failure.isEmpty();
    const QString log = QString::fromUtf8(ran.out);
    const QString producedPdf = work.filePath(jobName + QStringLiteral(".pdf"));
    // The work directory is the sandbox's to write: a link there to a file of the user's would be copied
    // into the cache as if TeX had made it. Only a regular file is taken.
    const QFileInfo produced(producedPdf);
    const bool succeeded = finished && ran.code == 0 && !produced.isSymLink() && produced.isFile()
                           && produced.size() <= kMaxPDFBytes;

    ensurePrivateDir(outDir);
    if (succeeded) {
        QFile::remove(pdfPath); // ensurePrivateDir already ensured outDir exists
        if (QFile::copy(producedPdf, pdfPath)) {
            result.ok = true;
            result.pdfPath = pdfPath;
            return result;
        }
        result.error = QStringLiteral("compiled but could not cache the PDF");
        return result;
    }

    result.log = ran.failure.isEmpty() ? tail(log, 25) : ran.failure;
    QFile failLog(failPath);
    if (failLog.open(QIODevice::WriteOnly))
        failLog.write(result.log.toUtf8());
    return result;
}

} // namespace noteAppC
