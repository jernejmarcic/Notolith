#include "Vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

namespace noteAppC {

namespace {
// Qt's UTF-8 decoder silently drops a leading byte-order mark, so the mark has
// to be detected on the bytes, not on the decoded string.
const QByteArray kBom = QByteArrayLiteral("\xEF\xBB\xBF");

// Obsidian refuses these in a file or folder name, wherever they came from.
const QString kInvalidNameChars = QStringLiteral("#^[]|:\\/");

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
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
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
    return contains(folder) ? folder : m_root;
}

QString Vault::uniqueAttachmentPath(const QString &folder, const QString &fileName)
{
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    const QDir dir(folder);
    QString path = dir.filePath(fileName);
    for (int n = 1; QFileInfo::exists(path); ++n) {
        path = dir.filePath(suffix.isEmpty() ? QStringLiteral("%1 %2").arg(base).arg(n)
                                             : QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(suffix));
    }
    return path;
}

Note Vault::decode(const QByteArray &bytes)
{
    Note note;
    note.bom = bytes.startsWith(kBom);
    QString text = QString::fromUtf8(note.bom ? bytes.mid(kBom.size()) : bytes);

    // Only a uniformly CRLF file is round-tripped as CRLF.
    // ponytail: a mixed-ending file is normalised to LF on save - the one case
    // where a save touches bytes it did not edit. Track per-line endings if a
    // real vault ever hits it.
    const bool hasCrlf = text.contains(QLatin1String("\r\n"));
    if (hasCrlf) {
        QString withoutCrlf = text;
        withoutCrlf.replace(QLatin1String("\r\n"), QString());
        note.crlf = !withoutCrlf.contains(QLatin1Char('\n'));
    }
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    note.text = text;
    return note;
}

QByteArray Vault::encode(const Note &note)
{
    QString text = note.text;
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
    return writeBytes(path, encode(note), error);
}

bool Vault::writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
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

// --- file operations -----------------------------------------------------

QString Vault::createNote(const QString &folderRelative, const QString &baseName,
                          QString *error, const QString &extension, const QString &text) const
{
    if (!isValid()) {
        if (error)
            *error = QStringLiteral("vault is not open");
        return {};
    }

    const QString folderAbsolute = folderRelative.isEmpty() ? m_root : absolute(folderRelative);
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
    if (!write(path, note, error))
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

    const QString parentAbsolute = parentRelative.isEmpty() ? m_root : absolute(parentRelative);
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

bool Vault::renamePath(const QString &absolutePath, const QString &newBaseName,
                       QString *newAbsolutePath, QString *error) const
{
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

    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("%1 does not exist").arg(absolutePath);
        return false;
    }

    // A note keeps its extension; a folder has none to keep.
    const QString newFileName = (info.isFile() && (isNote(absolutePath) || isEncryptedNote(absolutePath)))
                                    ? newBaseName + QLatin1Char('.') + info.suffix()
                                    : newBaseName;
    const QString newPath = QDir(info.absolutePath()).absoluteFilePath(newFileName);

    if (newPath == absolutePath) {
        if (newAbsolutePath)
            *newAbsolutePath = newPath;
        return true;
    }
    if (QFileInfo::exists(newPath)) {
        if (error)
            *error = QStringLiteral("%1 already exists").arg(newFileName);
        return false;
    }

    QFile file(absolutePath);
    if (!file.rename(newPath)) {
        if (error)
            *error = file.errorString();
        return false;
    }
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
        // ponytail: a fixed "build*/" heuristic, not a full .gitignore reader -
        // revisit if a real vault needs more.
        static const QRegularExpression buildDir(QStringLiteral("(^|/)build[^/]*/"));
        if (buildDir.match(relativePath).hasMatch())
            return true;
    }
    return false;
}

} // namespace noteAppC
