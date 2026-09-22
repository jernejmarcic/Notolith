#include "Vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace noteAppC {

namespace {
// Qt's UTF-8 decoder silently drops a leading byte-order mark, so the mark has
// to be detected on the bytes, not on the decoded string.
const QByteArray kBom = QByteArrayLiteral("\xEF\xBB\xBF");
} // namespace

Vault::Vault(const QString &rootPath)
    : m_root(QDir(rootPath).absolutePath())
{
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
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = encode(note);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace noteAppC
