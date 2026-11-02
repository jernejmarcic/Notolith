// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageDecoder.h"

#include "Bubblewrap.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <unistd.h>

namespace noteAppC::remoteimage {

namespace {

constexpr int kTimeoutMs = 10'000;

// The directory bubblewrap makes writable and the helper's home. The helper writes nothing; it is empty.
QString scratchDir()
{
    static QMutex lock;
    static QTemporaryDir *dir = nullptr; // lives to the end of the process
    QMutexLocker guard(&lock);
    if (!dir)
        dir = new QTemporaryDir;
    return dir->isValid() ? dir->path() : QString();
}

sandbox::Command helperCommand(const QString &helper)
{
    const QString dir = scratchDir();
    if (dir.isEmpty() || helper.isEmpty())
        return {};
    // The helper's own directory is bound read-only: a build tree is not under /usr.
    return sandbox::bubblewrapCommand(dir, helper, {}, {}, {QFileInfo(helper).absolutePath()});
}

} // namespace

QString helperPath()
{
#ifdef NOTEAPPC_DEV_ENV_OVERRIDES
    if (const QString overridden = qEnvironmentVariable("NOTEAPPC_IMAGE_HELPER"); !overridden.isEmpty())
        return overridden;
#endif
    const QString name = QStringLiteral("notolith-imagedecode");
    const QDir app(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {app.filePath(name), app.filePath(QStringLiteral("../libexec/notolith/") + name),
                                     QStringLiteral(NOTEAPPC_LIBEXECDIR "/") + name}) {
        if (QFileInfo(candidate).isExecutable())
            return QFileInfo(candidate).canonicalFilePath();
    }
    return {};
}

static QString decoderProbe()
{
    const QString helper = helperPath();
    if (helper.isEmpty())
        return QCoreApplication::translate("RemoteImage", "Not loaded: the image decoder (notolith-imagedecode) is not installed.");
    sandbox::Command command = helperCommand(helper);
    if (command.program.isEmpty())
        return QCoreApplication::translate("RemoteImage", "Not loaded: images from the web are decoded only inside bubblewrap's sandbox; install bubblewrap.");
    if (command.filterFD >= 0)
        ::close(command.filterFD);
    return {};
}

QString decoderUnavailable()
{
    // Asked on every load: the answer (a seccomp filter built and thrown away) is the same for the whole process
    static const QString answer = decoderProbe();
    return answer;
}

sandbox::Command decoderCommand()
{
    return helperCommand(helperPath());
}

DecodedImage decodeInSandbox(const QByteArray &bytes, ImageFormat format)
{
    const sandbox::Command command = decoderCommand();
    sandbox::Limits limits;
    limits.cpuSeconds = 20;
    limits.fileSize = 1024 * 1024;
    limits.openFiles = 256;
    // 16 megapixels is 61 MiB of pixels; the reader's copy, the converted one and the reply are at most
    // three of them, with Qt and its plugins on top.
    limits.addressSpace = rlim_t(3) * 1024 * 1024 * 1024;
    const sandbox::Outcome outcome = sandbox::run(command, limits, bytes, kTimeoutMs, kHeaderSize + qint64(kMaxPixels) * 4 + 64 * 1024);
    if (!outcome.failure.isEmpty())
        return {QImage(), QCoreApplication::translate("RemoteImage", "Not loaded: the image decoder %1.").arg(outcome.failure)};
    if (outcome.code != 0)
        return {QImage(), QCoreApplication::translate("RemoteImage", "Not loaded: the image decoder stopped.")};
    return validateReply(outcome.out, format);
}

} // namespace noteAppC::remoteimage
