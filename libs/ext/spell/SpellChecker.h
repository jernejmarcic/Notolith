#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class Hunspell;

namespace noteAppC {

/// Spelling through hunspell, with the dictionaries a Linux system already
/// has: `/usr/share/hunspell`, `/usr/share/myspell`, `hunspell/` under each
/// XDG data directory, and `$DICPATH`, which is hunspell's own variable.
///
/// A word is correct when any loaded dictionary accepts it, so a note that
/// mixes two languages is checked against both. Words the person added live
/// in `~/.config/noteAppC/dictionary.txt`, one per line.
///
/// Loading a dictionary takes tens of milliseconds, so the window builds one
/// of these off the UI thread, after the first paint. After that it is used
/// from the UI thread only.
class SpellChecker {
public:
    /// `languages` in hunspell's spelling, "en_GB", "sl_SI"; a bare "en"
    /// takes the first English dictionary there is. Missing ones are skipped.
    explicit SpellChecker(const QStringList &languages);
    ~SpellChecker();
    SpellChecker(const SpellChecker &) = delete;
    SpellChecker &operator=(const SpellChecker &) = delete;

    /// The dictionaries that loaded, by name. Empty means nothing is checked.
    QStringList languages() const;

    bool isCorrect(const QString &word) const;
    QStringList suggestions(const QString &word, int limit = 6) const;
    /// Accepts the word from now on, and remembers it for next time.
    void addWord(const QString &word);
    /// Whether the person added `word` (and so may take it back).
    bool isPersonal(const QString &word) const { return m_personal.contains(word.trimmed()); }
    /// Takes a word back out: wrong again from now on, and gone from the file.
    void removeWord(const QString &word);

    /// Where dictionaries are looked for, in order.
    static QStringList dictionaryDirectories();
    /// The languages the system asks for: LANGUAGE, then the locale.
    static QStringList systemLanguages();

private:
    struct Dictionary;
    std::vector<std::unique_ptr<Dictionary>> m_dictionaries;
    void savePersonal() const;
    QStringList m_personal;
    /// Words repeat, and asking hunspell costs microseconds each time.
    mutable QHash<QString, bool> m_known;
};

} // namespace noteAppC
