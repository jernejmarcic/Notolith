#include "GlyphStore.h"

#include <algorithm>
#include <set>

#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>

#include "InkDocument.h"
#include "Vault.h"

namespace noteAppC::hwfont {

namespace {
const QString kRoot = QStringLiteral("Handwriting");
}

GlyphStore::GlyphStore(const QString &vaultRoot, const QString &fontName) : m_vaultRoot(vaultRoot), m_fontName(fontName) {}

bool GlyphStore::isValidFontName(const QString &fontName)
{
    if (fontName.isEmpty() || fontName.startsWith(QLatin1Char('.')) || fontName.contains(QLatin1String("..")))
        return false;
    return std::none_of(fontName.begin(), fontName.end(), [](QChar c) {
        return c == QLatin1Char('/') || c == QLatin1Char('\\') || c.unicode() < 0x20 || c.unicode() == 0x7F
            || (c.unicode() >= 0x80 && c.unicode() < 0xA0); // C0, DEL and C1 controls (NUL included)
    });
}

QString GlyphStore::fontDirectory() const
{
    return isValid() ? m_vaultRoot + QLatin1Char('/') + kRoot + QLatin1Char('/') + m_fontName : QString();
}

QString GlyphStore::glyphDirectory(char32_t codepoint) const
{
    return isValid() ? fontDirectory() + QLatin1Char('/') + glyphDirectoryName(codepoint) : QString();
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

QString GlyphStore::nextSamplePath(char32_t codepoint) const
{
    const QString dir = glyphDirectory(codepoint);
    if (dir.isEmpty())
        return QString();
    const std::set<int> taken = sampleNumbers(dir);
    int n = 1;
    while (taken.count(n))
        ++n;
    return dir + QLatin1Char('/') + QString::number(n) + QStringLiteral(".ink");
}

bool GlyphStore::saveSample(char32_t codepoint, const ink::InkDocument &sample, const GlyphMetrics &metrics, QString *error)
{
    if (!isValid()) {
        if (error)
            *error = QStringLiteral("\"%1\" is not a usable font name").arg(m_fontName);
        return false;
    }
    ink::DocumentSettings settings = sample.settings();
    settings.extra.insert(QStringLiteral("glyph"),
                          QJsonObject{{QStringLiteral("codepoint"), int(codepoint)},
                                      {QStringLiteral("baseline"), metrics.baseline},
                                      {QStringLiteral("xHeight"), metrics.xHeight},
                                      {QStringLiteral("ascender"), metrics.ascender}});
    ink::InkDocument scratch; // the studio's own document stays as it was, modified flag and history included
    scratch.reset(settings, sample.items(), sample.extraTopLevel());

    // The vault's no-follow writer: creates the missing folders and refuses a link on the way, so a
    // Handwriting/ (or font folder) that is a symlink out of the vault is not written through.
    const Vault vault(m_vaultRoot);
    const QString path = nextSamplePath(codepoint);
    return vault.writeBytesNoFollow(vault.relative(path), ink::writeInk(scratch), error);
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
