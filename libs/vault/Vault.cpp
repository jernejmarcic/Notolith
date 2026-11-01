// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "Vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace noteAppC {

namespace {
// Qt's UTF-8 decoder silently drops a leading byte-order mark, so the mark has
// to be detected on the bytes, not on the decoded string.
const QByteArray kBom = QByteArrayLiteral("\xEF\xBB\xBF");

// Obsidian refuses these in a file or folder name, wherever they came from.
const QString kInvalidNameChars = QStringLiteral("#^[]|:\\/");

/// A name or folder part starting with a dot is hidden: the index, the file
/// tree and the links all skip it, so the note would seem to vanish.
/// Obsidian refuses such names too.
bool isHiddenName(const QString &relative)
{
    for (const QStringView part : QStringView(relative).split(QLatin1Char('/')))
        if (part.startsWith(QLatin1Char('.')) && part != QLatin1String("."))
            return true;
    return false;
}

const QString kHiddenNameError = QStringLiteral("a name starting with a dot would be hidden");
const QString kOutsideError = QStringLiteral("that would be outside the vault");

bool hasInvalidNameChar(const QString &name)
{
    for (const QChar &c : name) {
        if (kInvalidNameChars.contains(c))
            return true;
    }
    return false;
}

} // namespace

Vault::Vault(const QString &rootPath)
    : m_root(QDir(rootPath).absolutePath())
{
    loadIgnoreFilters();
}

bool Vault::isValid() const
{
    return !m_root.isEmpty() && QFileInfo(m_root).isDir();
}

QString Vault::name() const
{
    return QDir(m_root).dirName();
}

bool Vault::hasObsidianConfig() const
{
    return QFileInfo(m_root + QLatin1String("/.obsidian")).isDir();
}

bool Vault::isEncryptedNote(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("mdlock"), Qt::CaseInsensitive) == 0;
}

bool Vault::isNote(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0;
}

QString Vault::relative(const QString &absolutePath) const
{
    return QDir(m_root).relativeFilePath(absolutePath);
}

QString Vault::absolute(const QString &relativePath) const
{
    return QDir(m_root).absoluteFilePath(relativePath);
}

bool Vault::contains(const QString &absolutePath) const
{
    if (!isValid())
        return false;
    const QString path = QFileInfo(absolutePath).absoluteFilePath();
    return path == m_root || path.startsWith(m_root + QLatin1Char('/'));
}

QString Vault::confinedNotePath(const QString &relativePath) const
{
    if (!isValid() || relativePath.isEmpty() || QDir::isAbsolutePath(relativePath))
        return {};
    const QString path = QDir::cleanPath(absolute(relativePath));
    if (!isNote(path) || !contains(path))
        return {};
    const QFileInfo info(path);
    const QString real = info.exists() ? info.canonicalFilePath()
                                       : QFileInfo(info.path()).canonicalFilePath() + QLatin1Char('/') + info.fileName();
    const QString root = QFileInfo(m_root).canonicalFilePath();
    return real.startsWith(root + QLatin1Char('/')) ? path : QString();
}

QString Vault::pathInside(const QString &root, const QString &relative)
{
    if (QDir::isAbsolutePath(relative) || relative.contains(QChar(0)))
        return {};
    const QString cleanRoot = QDir::cleanPath(root);
    const QString path = QDir::cleanPath(cleanRoot + QLatin1Char('/') + relative);
    return path == cleanRoot || path.startsWith(cleanRoot + QLatin1Char('/')) ? path : QString();
}

bool Vault::isConfinedPath(const QString &relativePath, bool leafMayBeLink) const
{
    if (!isValid() || relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
        || relativePath.contains(QLatin1Char('\\')) || relativePath.contains(QChar(0)))
        return false;
    for (const QStringView part : QStringView(relativePath).split(QLatin1Char('/'))) {
        if (part.isEmpty() || part.startsWith(QLatin1Char('.')))
            return false; // empty, ".", "..", and the app's own hidden folders
    }
    const QFileInfo leaf(absolute(relativePath));
    if (!leafMayBeLink && leaf.isSymLink())
        return false;
    return resolvesInside(leaf.path());
}

bool Vault::resolvesInside(const QString &absolutePath) const
{
    return isValid() && resolvesInside(m_root, absolutePath);
}

bool Vault::resolvesInside(const QString &root, const QString &absolutePath)
{
    // The nearest ancestor that is there (a dangling link counts: its
    // canonical path is empty, so it fails below) decides where a new folder
    // or file would really land. Links on the way are followed.
    // Checked, not held open - a link swapped in between this and a write
    // wins the race; writeBytesNoFollow() closes that for a sync write.
    if (absolutePath.isEmpty())
        return false; // not "here"
    QFileInfo dir(QDir::cleanPath(absolutePath));
    while (!dir.exists() && !dir.isSymLink())
        dir = QFileInfo(dir.path());
    const QString real = dir.canonicalFilePath();
    const QString realRoot = root.isEmpty() ? QString() : QFileInfo(root).canonicalFilePath();
    return !real.isEmpty() && !realRoot.isEmpty()
        && (real == realRoot || real.startsWith(realRoot + QLatin1Char('/')));
}

QString Vault::attachmentFolderFor(const QString &setting, const QString &notePath) const
{
    const QString wanted = setting.trimmed();
    const QString noteFolder = QFileInfo(notePath).absolutePath();
    QString folder;
    if (wanted.isEmpty() || wanted == QLatin1String("/"))
        folder = m_root;
    else if (wanted == QLatin1String(".") || wanted == QLatin1String("./"))
        folder = noteFolder;
    else if (wanted.startsWith(QLatin1String("./")))
        folder = QDir::cleanPath(noteFolder + QLatin1Char('/') + wanted.mid(2));
    else
        folder = QDir::cleanPath(absolute(wanted));
    return contains(folder) && resolvesInside(folder) ? folder : m_root; // a linked folder is not inside
}

QString Vault::uniqueAttachmentPath(const QString &folder, const QString &fileName)
{
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    const QDir dir(folder);
    QString path = dir.filePath(fileName);
    // A link is taken too, a dangling one included: a write would go through it.
    const auto taken = [](const QString &p) { return QFileInfo::exists(p) || QFileInfo(p).isSymLink(); };
    for (int n = 1; taken(path); ++n) {
        path = dir.filePath(suffix.isEmpty() ? QStringLiteral("%1 %2").arg(base).arg(n)
                                             : QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(suffix));
    }
    return path;
}

Note Vault::decode(const QByteArray &bytes)
{
    Note note;
    note.bom = bytes.startsWith(kBom);
    // Stateless: a sequence cut off at the end is an error, not carried over.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = decoder(QByteArrayView(bytes).sliced(note.bom ? kBom.size() : 0));
    note.lossy = decoder.hasError();

    // A uniformly CRLF file is round-tripped as CRLF; a mixed one keeps its
    // text as read, for encode() to reuse around an edit.
    const bool hasCrlf = text.contains(QLatin1String("\r\n"));
    if (hasCrlf) {
        QString withoutCrlf = text;
        withoutCrlf.replace(QLatin1String("\r\n"), QString());
        note.crlf = !withoutCrlf.contains(QLatin1Char('\n'));
        if (!note.crlf)
            note.mixed = text;
        text.replace(QLatin1String("\r\n"), QLatin1String("\n")); // no match is still a scan of the note
        if (!note.crlf)
            note.mixedText = text;
    }

    note.text = text;
    return note;
}

namespace {
/// Where LF-text offset `at` starts in `raw`: the CR of a CRLF goes with its LF.
qsizetype rawOffset(const QString &raw, qsizetype at)
{
    qsizetype r = 0;
    for (qsizetype n = 0; n < at && r < raw.size(); ++n, ++r)
        if (raw.at(r) == QLatin1Char('\r') && r + 1 < raw.size() && raw.at(r + 1) == QLatin1Char('\n'))
            ++r;
    return r;
}

/// Mixed endings: the untouched head and tail as they were read, the edit between in LF.
QString keepMixedEndings(const Note &note)
{
    const QString &before = note.mixedText;
    const QString &now = note.text;
    qsizetype head = 0;
    while (head < before.size() && head < now.size() && before.at(head) == now.at(head))
        ++head;
    qsizetype tail = 0;
    while (tail < before.size() - head && tail < now.size() - head
           && before.at(before.size() - 1 - tail) == now.at(now.size() - 1 - tail))
        ++tail;
    return note.mixed.left(rawOffset(note.mixed, head)) + now.mid(head, now.size() - head - tail)
         + note.mixed.mid(rawOffset(note.mixed, before.size() - tail));
}
} // namespace

QByteArray Vault::encode(const Note &note)
{
    QString text = note.mixed.isEmpty() ? note.text : keepMixedEndings(note);
    if (note.crlf)
        text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    return note.bom ? kBom + text.toUtf8() : text.toUtf8();
}

bool Vault::read(const QString &path, Note &out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    out = decode(file.readAll());
    return true;
}

bool Vault::write(const QString &path, const Note &note, QString *error)
{
    if (note.lossy) {
        if (error)
            *error = QStringLiteral("the file is not valid UTF-8, so saving would change bytes that were not edited");
        return false;
    }
    return writeBytes(path, encode(note), error);
}

bool Vault::writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    // QSaveFile writes the target of a link, a dangling one too, and a vault can ship one.
    if (QFileInfo(path).isSymLink()) {
        if (error)
            *error = QStringLiteral("%1 is a link, so it was not written").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

namespace {
// Owns a descriptor; the walk below holds one per folder.
struct Fd {
    int fd = -1;
    explicit Fd(int f = -1) : fd(f) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() { if (fd >= 0) ::close(fd); }
    void reset(int f) { if (fd >= 0) ::close(fd); fd = f; }
};

QString errnoText(int err)
{
    return QString::fromLocal8Bit(std::strerror(err));
}
} // namespace

bool Vault::copyReplacing(const QString &source, const QString &target, QString *error)
{
    const auto fail = [error](const QString &what) {
        if (error)
            *error = what;
        return false;
    };
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly))
        return fail(in.errorString());
    // A temporary file beside the target, renamed over it once whole: the old file is never removed first.
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return fail(out.errorString());
    qint64 copied = 0;
    for (;;) {
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty())
            break;
        if (out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            return fail(out.errorString());
        }
        copied += chunk.size();
    }
    if (in.error() != QFileDevice::NoError || copied != in.size()) {
        out.cancelWriting();
        return fail(in.errorString());
    }
    if (!out.commit())
        return fail(out.errorString());
    return true;
}

bool Vault::isCleanRelativePath(const QString &relativePath)
{
    if (relativePath.isEmpty() || relativePath.contains(QLatin1Char('\\')) || relativePath.contains(QChar(0)))
        return false;
    for (const QStringView part : QStringView(relativePath).split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false; // empty (so also absolute), "." and ".."
    }
    return true;
}

namespace {
/// Opens the folder `parts` (all but the leaf) under `root`, one openat(O_NOFOLLOW) per component, so a link
/// anywhere on the way fails instead of leading out. `create` makes the missing folders in the descriptor already
/// held. Returns -1 with `*err` set (ENOENT when a folder is absent and `create` is off).
int openFolderNoFollow(const QString &root, const QStringList &parts, bool create, int *err, QString *failedPart)
{
    Fd dir(::open(QFile::encodeName(root).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (dir.fd < 0) {
        *err = errno;
        if (failedPart)
            failedPart->clear();
        return -1;
    }
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QByteArray name = parts.at(i).toUtf8();
        constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        int next = ::openat(dir.fd, name.constData(), flags);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(dir.fd, name.constData(), 0755) < 0 && errno != EEXIST) {
                *err = errno;
                if (failedPart)
                    *failedPart = parts.at(i);
                return -1;
            }
            next = ::openat(dir.fd, name.constData(), flags); // a link raced in is refused here
        }
        if (next < 0) {
            *err = errno;
            if (failedPart)
                *failedPart = parts.at(i);
            return -1;
        }
        dir.reset(next);
    }
    const int out = dir.fd;
    dir.fd = -1; // handed over
    return out;
}
} // namespace

bool Vault::writeBytesNoFollow(const QString &relativePath, const QByteArray &bytes, QString *error) const
{
    if (!isValid()) {
        if (error)
            *error = QStringLiteral("vault is not open");
        return false;
    }
    return writeBytesNoFollowUnder(m_root, relativePath, bytes, error);
}

bool Vault::writeBytesNoFollowUnder(const QString &root, const QString &relativePath, const QByteArray &bytes,
                                    QString *error, bool sync)
{
    const auto fail = [error](const QString &what) {
        if (error)
            *error = what;
        return false;
    };
    const QStringList parts = relativePath.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")
            || part.contains(QChar(0)))
            return fail(QStringLiteral("%1 is not a path inside the vault").arg(relativePath));
    }

    int err = 0;
    QString failedPart;
    Fd dir(openFolderNoFollow(root, parts, true, &err, &failedPart));
    if (dir.fd < 0) {
        if (failedPart.isEmpty())
            return fail(QStringLiteral("cannot open the vault: %1").arg(errnoText(err)));
        if (err == ELOOP || err == ENOTDIR)
            return fail(QStringLiteral("%1 is a link or not a folder, so %2 was not written")
                            .arg(failedPart, relativePath));
        return fail(QStringLiteral("cannot open %1: %2").arg(failedPart, errnoText(err)));
    }

    // Dot-prefixed so the scanner and indexer never see it. A leaf that is a
    // link is replaced by the rename, not followed.
    const QByteArray temp = ".tmp-" + QUuid::createUuid().toByteArray(QUuid::Id128);
    const QByteArray leaf = parts.last().toUtf8();
    Fd file(::openat(dir.fd, temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (file.fd < 0)
        return fail(QStringLiteral("cannot write %1: %2").arg(relativePath, errnoText(errno)));
    // Keep a file's own permissions, as QSaveFile does: a private 0600 note stays private.
    struct stat old {};
    if (::fstatat(dir.fd, leaf.constData(), &old, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(old.st_mode))
        ::fchmod(file.fd, old.st_mode & 07777);
    const auto cleanup = [&](int why) {
        file.reset(-1);
        ::unlinkat(dir.fd, temp.constData(), 0);
        return fail(QStringLiteral("cannot write %1: %2").arg(relativePath, errnoText(why)));
    };
    for (qsizetype done = 0; done < bytes.size();) {
        const ssize_t n = ::write(file.fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return cleanup(n < 0 ? errno : EIO);
        done += n;
    }
    if (sync && ::fsync(file.fd) < 0) // as QSaveFile does: the bytes are on disk before the name moves
        return cleanup(errno);
    file.reset(-1);
    if (::renameat(dir.fd, temp.constData(), dir.fd, leaf.constData()) < 0)
        return cleanup(errno);
    return true;
}

bool Vault::readFileNoFollowUnder(const QString &root, const QString &relativePath, QByteArray *out, qint64 maxBytes)
{
    out->clear();
    if (!isCleanRelativePath(relativePath))
        return false;
    const QStringList parts = relativePath.split(QLatin1Char('/'));
    int err = 0;
    Fd dir(openFolderNoFollow(root, parts, false, &err, nullptr));
    if (dir.fd < 0)
        return false;
    // O_NONBLOCK so that a named pipe in the place of a file cannot hold the open.
    Fd file(::openat(dir.fd, parts.last().toUtf8().constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.fd < 0)
        return false;
    struct stat info {};
    if (::fstat(file.fd, &info) < 0 || !S_ISREG(info.st_mode) || (maxBytes >= 0 && info.st_size > maxBytes))
        return false;
    QByteArray bytes;
    constexpr qsizetype chunk = 1 << 16;
    for (;;) {
        const qsizetype at = bytes.size();
        bytes.resize(at + chunk);
        const ssize_t n = ::read(file.fd, bytes.data() + at, size_t(chunk));
        if (n < 0 && errno == EINTR) {
            bytes.resize(at);
            continue;
        }
        if (n < 0)
            return false;
        bytes.resize(at + n);
        if (n == 0)
            break;
        if (maxBytes >= 0 && bytes.size() > maxBytes)
            return false; // grew while it was read
    }
    *out = std::move(bytes);
    return true;
}

bool Vault::removeNoFollowUnder(const QString &root, const QString &relativePath, QString *error)
{
    if (error)
        error->clear();
    if (!isCleanRelativePath(relativePath)) {
        if (error)
            *error = QStringLiteral("%1 is not a path inside the folder").arg(relativePath);
        return false;
    }
    const QStringList parts = relativePath.split(QLatin1Char('/'));
    int err = 0;
    Fd dir(openFolderNoFollow(root, parts, false, &err, nullptr));
    if (dir.fd < 0) {
        // A folder that is gone or a link on the way: nothing is removed, and only a link is worth saying.
        if (error && err != ENOENT)
            *error = QStringLiteral("%1 was not removed: %2").arg(relativePath, errnoText(err));
        return false;
    }
    // unlinkat removes a link itself, never what it points at; a folder is refused.
    if (::unlinkat(dir.fd, parts.last().toUtf8().constData(), 0) < 0) {
        if (error && errno != ENOENT)
            *error = QStringLiteral("%1 was not removed: %2").arg(relativePath, errnoText(errno));
        return false;
    }
    return true;
}

bool Vault::writeNoteNoFollow(const QString &relativePath, const Note &note, QString *error) const
{
    if (note.lossy) {
        if (error)
            *error = QStringLiteral("the file is not valid UTF-8, so saving would change bytes that were not edited");
        return false;
    }
    return writeBytesNoFollow(relativePath, encode(note), error);
}

// --- file operations -----------------------------------------------------

QString Vault::createNote(const QString &folderRelative, const QString &baseName,
                          QString *error, const QString &extension, const QString &text) const
{
    if (!isValid()) {
        if (error)
            *error = QStringLiteral("vault is not open");
        return {};
    }
    if (isHiddenName(folderRelative) || isHiddenName(baseName)) {
        if (error)
            *error = kHiddenNameError;
        return {};
    }

    // An absolute folder or one climbing with .. would land outside (a link's
    // target can be either).
    const QString folderAbsolute = QDir::cleanPath(folderRelative.isEmpty() ? m_root : absolute(folderRelative));
    if (!contains(folderAbsolute) || !resolvesInside(folderAbsolute) || baseName.contains(QLatin1Char('/'))) {
        if (error)
            *error = kOutsideError;
        return {};
    }
    if (!QDir().mkpath(folderAbsolute)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(folderAbsolute);
        return {};
    }

    const QDir folder(folderAbsolute);
    QString candidate = baseName;
    QString path;
    for (int suffix = 0;; ++suffix) {
        path = folder.absoluteFilePath(candidate + extension);
        if (!QFileInfo::exists(path))
            break;
        candidate = QStringLiteral("%1 %2").arg(baseName).arg(suffix + 1);
    }

    Note note;
    note.text = text;
    if (!writeNoteNoFollow(relative(path), note, error)) // a symlinked file is not written through
        return {};
    return path;
}

QString Vault::createFolder(const QString &parentRelative, const QString &baseName,
                            QString *error) const
{
    if (!isValid()) {
        if (error)
            *error = QStringLiteral("vault is not open");
        return {};
    }

    if (isHiddenName(parentRelative) || isHiddenName(baseName)) {
        if (error)
            *error = kHiddenNameError;
        return {};
    }

    const QString parentAbsolute = QDir::cleanPath(parentRelative.isEmpty() ? m_root : absolute(parentRelative));
    if (!contains(parentAbsolute) || !resolvesInside(parentAbsolute) || baseName.contains(QLatin1Char('/'))) {
        if (error)
            *error = kOutsideError;
        return {};
    }
    if (!QDir().mkpath(parentAbsolute)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(parentAbsolute);
        return {};
    }

    const QDir parent(parentAbsolute);
    QString candidate = baseName;
    QString path;
    for (int suffix = 0;; ++suffix) {
        path = parent.absoluteFilePath(candidate);
        if (!QFileInfo::exists(path))
            break;
        candidate = QStringLiteral("%1 %2").arg(baseName).arg(suffix + 1);
    }

    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(path);
        return {};
    }
    return path;
}

namespace {
/// An image's annotations sidecar (ImageAnnotations::sidecarPath: "<file name> annotations.md" beside it) goes with
/// the file it belongs to. Best effort, and never over a sidecar already at the new name. False when a sidecar was
/// there to move and could not be.
bool moveAnnotationSidecar(const QString &oldPath, const QString &newPath)
{
    const auto sidecar = [](const QString &path) {
        const QFileInfo info(path);
        return info.absolutePath() + QLatin1Char('/') + info.fileName() + QStringLiteral(" annotations.md");
    };
    const QString from = sidecar(oldPath);
    const QString to = sidecar(newPath);
    if (QFileInfo(newPath).isFile() && QFileInfo(from).isFile() && !QFileInfo::exists(to))
        return QFile::rename(from, to);
    return true;
}
} // namespace

bool Vault::renamePath(const QString &absolutePath, const QString &newBaseName,
                       QString *newAbsolutePath, QString *error, QString *warning) const
{
    const QString cleaned = QDir::cleanPath(absolutePath);
    if (!isValid() || !contains(cleaned) || cleaned == m_root) {
        if (error)
            *error = kOutsideError;
        return false;
    }
    if (newBaseName.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("name cannot be empty");
        return false;
    }
    if (hasInvalidNameChar(newBaseName)) {
        if (error)
            *error = QStringLiteral("name contains a character Obsidian does not allow: # ^ [ ] | : \\ /");
        return false;
    }
    if (isHiddenName(newBaseName)) {
        if (error)
            *error = kHiddenNameError;
        return false;
    }

    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("%1 does not exist").arg(absolutePath);
        return false;
    }

    // A file keeps its extension (the name is edited without it); a folder
    // has none to keep. A name typed with the same extension is not doubled.
    const QString dotSuffix = QLatin1Char('.') + info.suffix();
    const bool keepSuffix = info.isFile() && !info.suffix().isEmpty()
                            && ((isNote(absolutePath) || isEncryptedNote(absolutePath))
                                || !newBaseName.endsWith(dotSuffix, Qt::CaseInsensitive));
    const QString newFileName = keepSuffix ? newBaseName + dotSuffix : newBaseName;
    const QString newPath = QDir(info.absolutePath()).absoluteFilePath(newFileName);

    if (newPath == absolutePath) {
        if (newAbsolutePath)
            *newAbsolutePath = newPath;
        return true;
    }
    // On a case-insensitive filesystem (exFAT, NTFS, SMB) "Note.md" exists
    // when only "note.md" does: a change of case alone is no clash there.
    const bool caseOnly = isCaseOnlyChange(absolutePath, newPath);
    std::error_code sameError;
    if (QFileInfo::exists(newPath)
        && !(caseOnly && std::filesystem::equivalent(absolutePath.toStdString(), newPath.toStdString(), sameError))) {
        if (error)
            *error = QStringLiteral("%1 already exists").arg(newFileName);
        return false;
    }

    if (caseOnly) {
        // QFile::rename refuses a destination that "exists", which on such a
        // filesystem is the file itself, and Linux's vfat takes rename(2)
        // from "note.md" to "Note.md" for a rename onto itself: success,
        // nothing changed. Through a name in between it is two real renames
        // on every filesystem. That name is visible and no note: should both
        // the second step and the way back fail, the file is still in the
        // tree, and the error names it.
        const QString between = QDir(info.absolutePath())
                                    .absoluteFilePath(newFileName + QStringLiteral(".renaming-")
                                                      + QUuid::createUuid().toString(QUuid::Id128).left(8));
        std::error_code renameError;
        std::filesystem::rename(absolutePath.toStdString(), between.toStdString(), renameError);
        QString stranded;
        if (!renameError) {
            std::filesystem::rename(between.toStdString(), newPath.toStdString(), renameError);
            if (renameError) {
                std::error_code backError;
                std::filesystem::rename(between.toStdString(), absolutePath.toStdString(), backError);
                if (backError)
                    stranded = between;
            }
        }
        if (renameError) {
            if (error) {
                *error = QString::fromStdString(renameError.message());
                if (!stranded.isEmpty())
                    *error += QStringLiteral(" (the file is now %1)").arg(QFileInfo(stranded).fileName());
            }
            return false;
        }
    } else {
        QFile file(absolutePath);
        if (!file.rename(newPath)) {
            if (error)
                *error = file.errorString();
            return false;
        }
    }
    if (!moveAnnotationSidecar(absolutePath, newPath) && warning)
        *warning = QStringLiteral("its image annotations file could not be moved with it");
    if (newAbsolutePath)
        *newAbsolutePath = newPath;
    return true;
}

bool Vault::isCaseOnlyChange(const QString &oldPath, const QString &newPath)
{
    return oldPath != newPath && oldPath.compare(newPath, Qt::CaseInsensitive) == 0;
}

bool Vault::movePath(const QString &absolutePath, const QString &targetFolder,
                     QString *newAbsolutePath, QString *error, QString *warning) const
{
    const auto refuse = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QString source = QDir::cleanPath(absolutePath);
    const QString target = QDir::cleanPath(targetFolder);
    if (!isValid() || !contains(source) || !contains(target))
        return refuse(QStringLiteral("a move stays inside the vault"));
    const QFileInfo info(source);
    if (source == m_root || !info.exists())
        return refuse(QStringLiteral("%1 cannot be moved").arg(source));
    if (!QFileInfo(target).isDir())
        return refuse(QStringLiteral("%1 is not a folder").arg(target));
    // The vault's own bookkeeping folders and any hidden one are no place for a note.
    const QString targetRelative = relative(target);
    if (targetRelative != QLatin1String(".")
        && targetRelative.split(QLatin1Char('/')).first().startsWith(QLatin1Char('.')))
        return refuse(QStringLiteral("notes cannot be moved into a hidden folder"));
    if (target == source || target.startsWith(source + QLatin1Char('/')))
        return refuse(QStringLiteral("a folder cannot be moved into itself"));

    const QString newPath = target + QLatin1Char('/') + info.fileName();
    if (newPath == source) {
        if (newAbsolutePath)
            *newAbsolutePath = newPath;
        return true;
    }
    if (QFileInfo::exists(newPath))
        return refuse(QStringLiteral("%1 already exists in that folder").arg(info.fileName()));
    QFile file(source);
    if (!file.rename(newPath))
        return refuse(file.errorString());
    if (!moveAnnotationSidecar(source, newPath) && warning)
        *warning = QStringLiteral("its image annotations file could not be moved with it");
    if (newAbsolutePath)
        *newAbsolutePath = newPath;
    return true;
}

bool Vault::moveToTrash(const QString &absolutePath, QString *error) const
{
    if (!isValid() || !contains(absolutePath)) {
        if (error)
            *error = QStringLiteral("%1 is not inside the vault").arg(absolutePath);
        return false;
    }

    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("%1 does not exist").arg(absolutePath);
        return false;
    }

    // ".trash/<same relative folder>/<name>", mirroring Obsidian's default.
    const QString relativeFolder = QFileInfo(relative(absolutePath)).path();
    const QString trashFolder = relativeFolder == QLatin1String(".")
                                    ? m_root + QLatin1String("/.trash")
                                    : m_root + QLatin1String("/.trash/") + relativeFolder;
    // A link as .trash, or on the way below it, would carry the file out of the vault.
    QString walked = m_root + QLatin1String("/.trash");
    bool linked = QFileInfo(walked).isSymLink();
    for (const QString &part : relativeFolder == QLatin1String(".") ? QStringList() : relativeFolder.split(QLatin1Char('/'))) {
        walked += QLatin1Char('/') + part;
        linked = linked || QFileInfo(walked).isSymLink();
    }
    if (linked || !resolvesInside(trashFolder)) {
        if (error)
            *error = QStringLiteral("the trash folder is a link or outside the vault, so %1 was not moved").arg(absolutePath);
        return false;
    }
    if (!QDir().mkpath(trashFolder)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(trashFolder);
        return false;
    }

    const QDir trash(trashFolder);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    QString candidate = info.fileName();
    QString target = trash.absoluteFilePath(candidate);
    for (int n = 1; QFileInfo::exists(target); ++n) {
        candidate = suffix.isEmpty() ? QStringLiteral("%1 %2").arg(base).arg(n)
                                     : QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(suffix);
        target = trash.absoluteFilePath(candidate);
    }

    QDir mover;
    if (!mover.rename(absolutePath, target)) {
        if (error)
            *error = QStringLiteral("cannot move %1 to trash").arg(absolutePath);
        return false;
    }
    return true;
}

// --- ignore filters --------------------------------------------------------

void Vault::loadIgnoreFilters()
{
    m_ignorePrefixes.clear();
    m_ignoreRegexes.clear();
    m_filtersConfigured = false;

    QFile file(m_root + QLatin1String("/.obsidian/app.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonArray filters =
        QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("userIgnoreFilters")).toArray();
    if (filters.isEmpty())
        return;
    m_filtersConfigured = true;

    // Each entry is either a plain path prefix, or a JavaScript regex literal
    // like "/(^|\/)node_modules\//".
    static const QRegularExpression literal(QStringLiteral("^/(.*)/([a-zA-Z]*)$"));
    for (const QJsonValue &value : filters) {
        const QString entry = value.toString();
        if (entry.isEmpty())
            continue;

        const auto match = literal.match(entry);
        if (!match.hasMatch()) {
            m_ignorePrefixes.append(entry);
            continue;
        }

        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        const QString flags = match.captured(2);
        if (flags.contains(QLatin1Char('i')))
            options |= QRegularExpression::CaseInsensitiveOption;
        if (flags.contains(QLatin1Char('s')))
            options |= QRegularExpression::DotMatchesEverythingOption;
        if (flags.contains(QLatin1Char('m')))
            options |= QRegularExpression::MultilineOption;

        QRegularExpression re(match.captured(1), options);
        if (re.isValid())
            m_ignoreRegexes.append(re);
        else
            m_ignorePrefixes.append(entry); // not valid PCRE either; treat literally
    }
}

bool Vault::isIgnored(const QString &relativePath) const
{
    for (const QString &prefix : m_ignorePrefixes) {
        if (relativePath.startsWith(prefix))
            return true;
    }
    for (const QRegularExpression &re : m_ignoreRegexes) {
        if (re.match(relativePath).hasMatch())
            return true;
    }

    if (!m_filtersConfigured) {
        // No configured filters: fall back to skipping the obvious build
        // artefact directories rather than sweeping them into the index.
        // Limit: a fixed "build*/" heuristic, not a full .gitignore reader -
        // revisit if a real vault needs more.
        static const QRegularExpression buildDir(QStringLiteral("(^|/)build[^/]*/"));
        if (buildDir.match(relativePath).hasMatch())
            return true;
    }
    return false;
}

} // namespace noteAppC
