#include "TexCompiler.h"

#include "SyscallFilter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <sys/resource.h>
#include <unistd.h>

namespace noteAppC {

namespace {

/// `$XDG_CACHE_HOME/noteAppC/tex`, or `~/.cache/noteAppC/tex` when the
/// variable is unset - the same fallback QStandardPaths itself uses, spelled
/// out because GenericCacheLocation folds in the application name and this
/// cache is meant to survive switching how noteAppC is invoked.
QString cacheRoot()
{
    QString base = qEnvironmentVariable("XDG_CACHE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/noteAppC/tex");
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

    QProcess process;
    process.setWorkingDirectory(work.path());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::MergedChannels);
    int filterFD = -1;
    QString program = engine;
    QStringList arguments = {QStringLiteral("-no-shell-escape"), QStringLiteral("-interaction=nonstopmode"),
                             QStringLiteral("-halt-on-error"), QStringLiteral("-jobname=") + jobName,
                             inputPath};
    // Where bubblewrap exists, the engine sees the system and its own
    // directory and nothing else: no home, no other drive, no socket, no
    // network. -no-shell-escape stops \write18, but \input can still read
    // any file the user can and set it into the PDF.
    if (const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap")); !bwrap.isEmpty()) {
        const QString dir = work.path();
        arguments = QStringList{QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
                                QStringLiteral("--ro-bind"), QStringLiteral("/etc"), QStringLiteral("/etc"),
                                QStringLiteral("--ro-bind-try"), QStringLiteral("/var/lib/texmf"),
                                QStringLiteral("/var/lib/texmf"),
                                QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
                                QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
                                QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
                                QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
                                QStringLiteral("--proc"), QStringLiteral("/proc"),
                                QStringLiteral("--dev"), QStringLiteral("/dev"),
                                QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                                QStringLiteral("--bind"), dir, dir,
                                QStringLiteral("--setenv"), QStringLiteral("HOME"), dir,
                                QStringLiteral("--unshare-all"), QStringLiteral("--die-with-parent"),
                                QStringLiteral("--new-session"), QStringLiteral("--chdir"), dir,
                                QStringLiteral("--"), engine}
                  + arguments;
        program = bwrap;
        filterFD = sandbox::addSyscallFilter(arguments);
    } else {
        // Refused rather than run bare: \input would read any file of the
        // user's into the PDF.
        result.error = QStringLiteral("TeX runs only inside bubblewrap's sandbox; install bubblewrap");
        return result;
    }
    // A memory and CPU ceiling in the child, applied between fork and exec -
    // a runaway or hostile document is capped before it can touch the
    // machine noteAppC runs on, not after.
    process.setChildProcessModifier([filterFD] {
        rlimit memory{2ULL * 1024 * 1024 * 1024, 2ULL * 1024 * 1024 * 1024};
        setrlimit(RLIMIT_AS, &memory);
        rlimit cpu{25, 30};
        setrlimit(RLIMIT_CPU, &cpu);
        sandbox::keepInChild(filterFD);
    });
    process.start(program, arguments);
    // The fork has happened; bwrap holds its own copy of the filter.
    if (filterFD >= 0)
        close(filterFD);

    bool finished = process.waitForFinished(30000);
    if (!finished) {
        process.kill();
        process.waitForFinished(2000);
    }

    const QString log = QString::fromUtf8(process.readAll());
    const QString producedPdf = work.filePath(jobName + QStringLiteral(".pdf"));
    const bool succeeded = finished && process.exitStatus() == QProcess::NormalExit
                           && process.exitCode() == 0 && QFile::exists(producedPdf);

    QDir().mkpath(outDir);
    if (succeeded) {
        QFile::remove(pdfPath); // mkpath already ensured outDir exists
        if (QFile::copy(producedPdf, pdfPath)) {
            result.ok = true;
            result.pdfPath = pdfPath;
            return result;
        }
        result.error = QStringLiteral("compiled but could not cache the PDF");
        return result;
    }

    result.log = tail(log, 25);
    QFile failLog(failPath);
    if (failLog.open(QIODevice::WriteOnly))
        failLog.write(result.log.toUtf8());
    return result;
}

} // namespace noteAppC
