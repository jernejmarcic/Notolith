#pragma once

#include <QString>

#include <optional>

namespace noteAppC {

/// Where a book was left: the chapter, the scroll offset within it, and the
/// font scale it was read at.
struct EpubPosition {
    int spineIndex = 0;
    int scrollWithinChapter = 0; ///< logical pixels, at fontPercent below
    int fontPercent = 100;
    bool paged = false; ///< continuous scroll vs paged reading - see EpubView::setPagedMode

    friend bool operator==(const EpubPosition &, const EpubPosition &) = default;
};

/// Remembers a reading position per book, the way Settings::notePosition()
/// remembers a note's caret and scroll - "outside the vault", in the same
/// `~/.config/notolith` directory, never inside it. A plain, self-contained
/// INI keyed by the book's own path rather than a new key in Settings
/// itself: this module does not otherwise depend on libs/settings, and a
/// second small file next to settings.ini/vaults.ini costs nothing that
/// depending on Settings for one lookup would not (see the report for the
/// alternative and why this one was picked).
class EpubPositionStore {
public:
    /// Unset when `bookPath` was never recorded.
    static std::optional<EpubPosition> get(const QString &bookPath);
    static void set(const QString &bookPath, const EpubPosition &position);

private:
    static QString iniPath();
};

} // namespace noteAppC
