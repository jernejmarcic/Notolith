// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "GlyphStore.h"

#include <algorithm>
#include <set>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include "InkDocument.h"
#include "Vault.h"

namespace noteAppC::hwfont {

namespace {
const QString kRoot = QStringLiteral("Handwriting");

// Obsidian's own set of characters a name may not hold (libs/vault/Vault.cpp, kInvalidNameChars, in an
// anonymous namespace so not callable from here): keep the two in step. A vault syncs to other
// systems and Obsidian, so a font folder must be a name they all accept.
const QString kVaultInvalidNameChars = QStringLiteral("#^[]|:\\/");

bool isReservedWindowsName(const QString &name)
{
    // CON.txt is as reserved as CON: judge the part before the first dot.
    const QString base = name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"));
    return pattern.match(base).hasMatch();
}

bool hasLoneSurrogate(const QString &name)
{
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c.isHighSurrogate() && i + 1 < name.size() && name.at(i + 1).isLowSurrogate())
            ++i;
        else if (c.isSurrogate())
            return true;
    }
    return false;
}
} // namespace

GlyphStore::GlyphStore(const QString &vaultRoot, const QString &fontName) : m_vaultRoot(vaultRoot), m_fontName(fontName) {}

bool GlyphStore::isValidFontName(const QString &fontName)
{
    if (fontName.isEmpty() || fontName.startsWith(QLatin1Char('.')) || fontName.contains(QLatin1String("..")))
        return false;
    if (fontName.endsWith(QLatin1Char('.')) || fontName.endsWith(QLatin1Char(' ')) || isReservedWindowsName(fontName)
        || hasLoneSurrogate(fontName))
        return false;
    return std::none_of(fontName.begin(), fontName.end(), [](QChar c) {
        return kVaultInvalidNameChars.contains(c) || c.unicode() < 0x20 || c.unicode() == 0x7F
            || (c.unicode() >= 0x80 && c.unicode() < 0xA0); // C0, DEL and C1 controls (NUL included)
    });
}

QString GlyphStore::fontDirectory() const
{
    return isValid() ? m_vaultRoot + QLatin1Char('/') + kRoot + QLatin1Char('/') + m_fontName : QString();
}

QString GlyphStore::glyphDirectory(char32_t codepoint) const
{
    return isValid() && isValidCodepoint(codepoint) ? fontDirectory() + QLatin1Char('/') + glyphDirectoryName(codepoint) : QString();
}

namespace {
/// The n of every <n>.ink in `dir` (digits only, so "x.ink" or "1.ink.bak" never count).
std::set<int> sampleNumbers(const QString &dir)
{
    static const QRegularExpression pattern(QStringLiteral("^([1-9][0-9]{0,8})\\.ink$"));
    std::set<int> numbers;
    for (const QString &name : QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        const auto match = pattern.match(name);
        if (match.hasMatch())
            numbers.insert(match.captured(1).toInt());
    }
    return numbers;
}
} // namespace

int GlyphStore::sampleCount(char32_t codepoint) const
{
    const QString dir = glyphDirectory(codepoint);
    return dir.isEmpty() ? 0 : int(sampleNumbers(dir).size());
}

namespace {
int lowestFree(const QString &dir)
{
    const std::set<int> taken = sampleNumbers(dir);
    int n = 1;
    while (taken.count(n))
        ++n;
    return n;
}

QString samplePathFor(const QString &dir, int n)
{
    return dir + QLatin1Char('/') + QString::number(n) + QStringLiteral(".ink");
}

/// Moves `from` to `to` unless `to` exists: 0, EEXIST, or another errno. renameat2(RENAME_NOREPLACE) is
/// atomic; where the kernel or file system lacks it (EINVAL, ENOSYS, ENOTSUP) it checks, then renames.
int renameNoReplace(const QString &from, const QString &to)
{
    const QByteArray a = QFile::encodeName(from), b = QFile::encodeName(to);
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
        return errno;
#endif
    // Limit: check-then-rename leaves a window of microseconds in which a sample written by another
    // process or a sync is replaced; only on file systems without RENAME_NOREPLACE (FAT, some network mounts).
    if (QFileInfo(to).isSymLink() || QFileInfo::exists(to))
        return EEXIST;
    return ::rename(a.constData(), b.constData()) == 0 ? 0 : errno;
}
} // namespace

QString GlyphStore::nextSamplePath(char32_t codepoint) const
{
    const QString dir = glyphDirectory(codepoint);
    return dir.isEmpty() ? QString() : samplePathFor(dir, lowestFree(dir));
}

bool GlyphStore::saveSample(char32_t codepoint, const ink::InkDocument &sample, const GlyphMetrics &metrics, QString *error)
{
    const auto fail = [error](const QString &what) {
        if (error)
            *error = what;
        return false;
    };
    if (!isValid())
        return fail(QStringLiteral("\"%1\" is not a usable font name").arg(m_fontName));
    if (!isValidCodepoint(codepoint))
        return fail(QStringLiteral("U+%1 is not a Unicode scalar value").arg(uint(codepoint), 0, 16));
    ink::DocumentSettings settings = sample.settings();
    settings.extra.insert(QStringLiteral("glyph"),
                          QJsonObject{{QStringLiteral("codepoint"), qint64(codepoint)},
                                      {QStringLiteral("baseline"), metrics.baseline},
                                      {QStringLiteral("xHeight"), metrics.xHeight},
                                      {QStringLiteral("ascender"), metrics.ascender}});
    ink::InkDocument scratch; // the studio's own document stays as it was, modified flag and history included
    scratch.reset(settings, sample.items(), sample.extraTopLevel());

    // The vault's no-follow writer: creates the missing folders and refuses a link on the way, so a
    // Handwriting/ (or font folder) that is a symlink out of the vault is not written through. It replaces
    // its target, so the bytes go to a unique dot-prefixed name first (never counted as a sample) and are
    // then moved to the lowest free <n>.ink without replacing anything: a sample another studio or a sync
    // wrote in the meantime is kept and this one takes the next number.
    const Vault vault(m_vaultRoot);
    const QString dir = glyphDirectory(codepoint);
    const QString part = dir + QStringLiteral("/.part-") + QUuid::createUuid().toString(QUuid::Id128);
    if (!vault.writeBytesNoFollow(vault.relative(part), ink::writeInk(scratch), error))
        return false;
    constexpr int kMaxAttempts = 10000;
    int n = lowestFree(dir);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt, ++n) {
        const int result = renameNoReplace(part, samplePathFor(dir, n));
        if (result == 0)
            return true;
        if (result != EEXIST) {
            QFile::remove(part);
            return fail(QStringLiteral("cannot write sample %1: %2").arg(n).arg(QString::fromLocal8Bit(std::strerror(result))));
        }
    }
    QFile::remove(part);
    return fail(QStringLiteral("no free sample number under %1").arg(dir));
}

std::vector<std::pair<GlyphTarget, int>> GlyphStore::progress(const std::vector<GlyphTarget> &set) const
{
    std::vector<std::pair<GlyphTarget, int>> result;
    result.reserve(set.size());
    for (const GlyphTarget &target : set)
        result.emplace_back(target, sampleCount(target.codepoint));
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        const bool aNeeds = a.second < a.first.wanted, bNeeds = b.second < b.first.wanted;
        return aNeeds != bNeeds ? aNeeds : a.first.codepoint < b.first.codepoint;
    });
    return result;
}

ink::DocumentSettings GlyphStore::samplePage()
{
    ink::DocumentSettings page;
    page.layout = ink::LayoutKind::Fixed;
    page.format = {400, 400, 96};
    page.extent = QRectF(0, 0, 400, 400);
    page.background.color = Qt::white;
    page.background.pattern = ink::Pattern::None;
    return page;
}

GlyphMetrics GlyphStore::defaultMetrics()
{
    return {280, 160, 100};
}

} // namespace noteAppC::hwfont
