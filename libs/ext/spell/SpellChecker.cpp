// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpellChecker.h"

#include "ConfigFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStringEncoder>

#include <hunspell.hxx>

namespace noteAppC {

namespace {

QString personalDictionaryPath()
{
    return configFilePath(QStringLiteral("dictionary.txt"));
}

} // namespace

/// One loaded dictionary, and the encoding its files are in: older ones are
/// ISO-8859, and hunspell wants words in the dictionary's own encoding.
struct SpellChecker::Dictionary {
    QString name;
    std::unique_ptr<Hunspell> hunspell;
    bool utf8 = true;
    QStringEncoder encoder;
    QStringDecoder decoder;

    std::string encode(const QString &word)
    {
        if (utf8)
            return word.toStdString();
        const QByteArray bytes = encoder.encode(word);
        return std::string(bytes.constData(), size_t(bytes.size()));
    }
    QString decode(const std::string &word)
    {
        if (utf8)
            return QString::fromStdString(word);
        return decoder.decode(QByteArrayView(word.data(), qsizetype(word.size())));
    }
};

QStringList SpellChecker::dictionaryDirectories()
{
    QStringList directories;
    for (const QString &path : qEnvironmentVariable("DICPATH").split(QLatin1Char(':'), Qt::SkipEmptyParts))
        directories << path;
    for (const QString &data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        directories << data + QStringLiteral("/hunspell");
    directories << QStringLiteral("/usr/share/hunspell") << QStringLiteral("/usr/share/myspell")
                << QStringLiteral("/usr/share/myspell/dicts");
    directories.removeDuplicates();
    return directories;
}

QStringList SpellChecker::systemLanguages()
{
    QStringList languages;
    for (QString language : QLocale::system().uiLanguages()) {
        language.replace(QLatin1Char('-'), QLatin1Char('_'));
        languages << language;
    }
    languages.removeDuplicates();
    return languages;
}

SpellChecker::SpellChecker(const QStringList &languages)
{
    const QStringList directories = dictionaryDirectories();
    QStringList loaded;
    for (const QString &language : languages) {
        // An exact dictionary, or for a bare language the first of its
        // regions: "en" finds en_GB.dic or en_US.dic, whichever is there.
        // A bare language already covered by one of its regions needs no
        // second dictionary: "en" after en_GB would load en_AG, which on Arch
        // is a symlink to the same en_GB-large files.
        const bool bare = !language.contains(QLatin1Char('_'));
        if (bare && std::any_of(loaded.cbegin(), loaded.cend(), [&](const QString &name) {
                return name.startsWith(language + QLatin1Char('_'));
            }))
            continue;
        QString aff;
        QString dic;
        for (const QString &directory : directories) {
            const QDir dir(directory);
            const QStringList names =
                !bare ? QStringList{language + QStringLiteral(".dic")}
                    : dir.entryList({language + QStringLiteral("_*.dic")}, QDir::Files, QDir::Name);
            for (const QString &name : names) {
                const QString base = dir.filePath(QFileInfo(name).completeBaseName());
                if (QFileInfo::exists(base + QStringLiteral(".dic"))
                    && QFileInfo::exists(base + QStringLiteral(".aff"))) {
                    dic = base + QStringLiteral(".dic");
                    aff = base + QStringLiteral(".aff");
                    break;
                }
            }
            if (!dic.isEmpty())
                break;
        }
        const QString name = QFileInfo(dic).completeBaseName();
        if (dic.isEmpty() || loaded.contains(name))
            continue;

        auto dictionary = std::make_unique<Dictionary>();
        dictionary->name = name;
        dictionary->hunspell = std::make_unique<Hunspell>(QFile::encodeName(aff).constData(),
                                                          QFile::encodeName(dic).constData());
        const QString encoding = QString::fromStdString(dictionary->hunspell->get_dict_encoding());
        dictionary->utf8 = encoding.compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) == 0;
        if (!dictionary->utf8) {
            // An encoding Qt cannot convert would feed hunspell garbage; such
            // a dictionary is better left out than trusted.
            dictionary->encoder = QStringEncoder(encoding.toLatin1().constData());
            dictionary->decoder = QStringDecoder(encoding.toLatin1().constData());
            if (!dictionary->encoder.isValid() || !dictionary->decoder.isValid())
                continue;
        }
        loaded << name;
        m_dictionaries.push_back(std::move(dictionary));
    }

    QFile personal(personalDictionaryPath());
    if (personal.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray &line : personal.readAll().split('\n')) {
            const QString word = QString::fromUtf8(line).trimmed();
            if (!word.isEmpty())
                m_personal << word;
        }
    }
    for (const QString &word : std::as_const(m_personal)) {
        for (const auto &dictionary : m_dictionaries)
            dictionary->hunspell->add(dictionary->encode(word));
    }
}

SpellChecker::~SpellChecker() = default;

QStringList SpellChecker::languages() const
{
    QStringList names;
    for (const auto &dictionary : m_dictionaries)
        names << dictionary->name;
    return names;
}

bool SpellChecker::isCorrect(const QString &word) const
{
    if (m_dictionaries.empty() || word.isEmpty())
        return true; // nothing to check against: nothing is wrong
    const auto cached = m_known.constFind(word);
    if (cached != m_known.constEnd())
        return *cached;
    bool correct = false;
    for (const auto &dictionary : m_dictionaries) {
        if (dictionary->hunspell->spell(dictionary->encode(word))) {
            correct = true;
            break;
        }
    }
    m_known.insert(word, correct);
    return correct;
}

QStringList SpellChecker::suggestions(const QString &word, int limit) const
{
    QStringList out;
    for (const auto &dictionary : m_dictionaries) {
        for (const std::string &suggestion : dictionary->hunspell->suggest(dictionary->encode(word))) {
            const QString decoded = dictionary->decode(suggestion);
            if (!out.contains(decoded))
                out << decoded;
            if (out.size() >= limit)
                return out;
        }
    }
    return out;
}

void SpellChecker::addWord(const QString &word)
{
    const QString trimmed = word.trimmed();
    if (trimmed.isEmpty() || m_personal.contains(trimmed))
        return;
    m_personal << trimmed;
    for (const auto &dictionary : m_dictionaries)
        dictionary->hunspell->add(dictionary->encode(trimmed));
    m_known.insert(trimmed, true);

    savePersonal();
}

void SpellChecker::removeWord(const QString &word)
{
    const QString trimmed = word.trimmed();
    if (m_personal.removeAll(trimmed) == 0)
        return;
    for (const auto &dictionary : m_dictionaries)
        dictionary->hunspell->remove(dictionary->encode(trimmed));
    m_known.remove(trimmed); // not set to false: hunspell may still say yes
    savePersonal();
}

void SpellChecker::savePersonal() const
{
    // The whole list is rewritten, atomically: a file that is half written
    // when the power goes would lose every word in it.
    const QString path = personalDictionaryPath();
    ensurePrivateDir(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    file.write(m_personal.join(QLatin1Char('\n')).toUtf8() + '\n');
    file.commit();
}

} // namespace noteAppC
