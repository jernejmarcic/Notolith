#include "SyncScanner.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>

#include "SyncCrypto.h"
#include "SyncJournal.h"
#include "Vault.h"

namespace noteAppC {

namespace {

/// Same rule as Indexer::isIgnored (libs/index/Indexer.cpp): any
/// dot-prefixed path segment. Duplicated rather than linking noteAppC_index
/// for one three-line static - see SyncScanner.h.
bool dotIgnored(const QString &relativePath)
{
    for (const QStringView part : QStringView(relativePath).split(QLatin1Char('/'))) {
        if (part.startsWith(QLatin1Char('.')))
            return true;
    }
    return false;
}

QByteArray hashFile(const QString &absolutePath, const QByteArray &contentKey)
{
    QFile f(absolutePath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return SyncCrypto::contentAddress(contentKey, f.readAll());
}

/// The non-blank lines of `text` with how often each occurs, and their count.
QHash<QString, int> lineBag(const QString &text, int *total)
{
    QHash<QString, int> bag;
    *total = 0;
    for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty())
            continue;
        ++bag[line.toString()];
        ++*total;
    }
    return bag;
}

} // namespace

QList<SyncScanner::Change> SyncScanner::scan(const Vault &vault, const SyncJournal &journal,
                                             const QByteArray &contentKey)
{
    QList<Change> changes;
    if (!vault.isValid())
        return changes;

    QHash<QString, SyncJournal::Entity> known; // path -> entity, for entities not yet accounted for
    for (const SyncJournal::Entity &entity : journal.allEntities()) {
        if (!entity.deleted)
            known.insert(entity.path, entity);
    }

    QSet<QString> seen;
    // NoSymLinks: a link to a file is a Files entry too, and its target - maybe
    // outside the vault - would be pushed as a note's own content.
    QDirIterator it(vault.root(), QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString absolute = it.next();
        const QString relative = vault.relative(absolute);
        if (dotIgnored(relative) || vault.isIgnored(relative))
            continue;
        const QFileInfo info(absolute);
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        const qint64 size = info.size();
        seen.insert(relative);

        const auto existingIt = known.find(relative);
        if (existingIt != known.end()) {
            // Copied out before erase(): QHash::erase() invalidates the
            // iterator immediately, and this entry's fields were still
            // needed right below - a bug caught by
            // SyncScannerTest.NewNoteIsCreatedAndUnchangedNoteIsSkipped
            // reading them straight after erase() and getting whatever
            // the freed/reused node happened to hold.
            const SyncJournal::Entity existing = existingIt.value();
            known.erase(existingIt);
            if (existing.localMtime == mtime && existing.localSize == size)
                continue; // cheap path: stat matched, no content read at all
            const QByteArray hash = hashFile(absolute, contentKey);
            if (hash == existing.localHash)
                continue; // touched but byte-identical (e.g. a re-save with no change)
            Change change;
            change.kind = Change::Modified;
            change.entityID = existing.entityID;
            change.path = relative;
            change.entityType = existing.entityType;
            change.mtime = mtime;
            change.size = size;
            change.hash = hash;
            changes.append(change);
            continue;
        }

        // No entity at this path: either new, or the far side of a rename
        // still to be matched once every present file has been seen.
        Change change;
        change.kind = Change::Created;
        change.path = relative;
        change.entityType = Vault::isNote(absolute) ? QStringLiteral("note") : QStringLiteral("attachment");
        change.mtime = mtime;
        change.size = size;
        change.hash = hashFile(absolute, contentKey);
        changes.append(change);
    }

    // Whatever is left in `known` was not seen on disk this pass: either
    // deleted, or the source side of a rename. Match by content hash
    // against this scan's Created entries first (ambiguous only when two
    // files are byte-identical and both moved in the same pass - picked
    // arbitrarily, see docs/SYNC.md).
    for (auto changeIt = changes.begin(); changeIt != changes.end(); ++changeIt) {
        if (changeIt->kind != Change::Created || changeIt->hash.isEmpty())
            continue;
        for (auto knownIt = known.begin(); knownIt != known.end(); ++knownIt) {
            if (knownIt->localHash != changeIt->hash || knownIt->entityType != changeIt->entityType)
                continue;
            changeIt->kind = Change::Renamed;
            changeIt->oldPath = knownIt->path;
            changeIt->entityID = knownIt->entityID;
            known.erase(knownIt);
            break;
        }
    }

    // A note renamed and edited in the same scan has no exact match: compare
    // the new notes with the vanished ones line by line (Dice over the
    // non-blank lines) and take a pair that is alike and clearly the best for
    // that file. A wrong pair hands the new note the old one's identity;
    // nothing is lost either way.
    // ponytail: only when new * vanished notes <= 64, a folder of edited notes
    // moved at once falls back to delete + create; raise it if that turns up.
    QList<qsizetype> created;
    for (qsizetype i = 0; i < changes.size(); ++i) {
        if (changes.at(i).kind == Change::Created && changes.at(i).entityType == QStringLiteral("note"))
            created.append(i);
    }
    QList<SyncJournal::Entity> vanished;
    for (const SyncJournal::Entity &gone : std::as_const(known)) {
        if (gone.entityType == QStringLiteral("note"))
            vanished.append(gone);
    }
    if (!created.isEmpty() && !vanished.isEmpty() && created.size() * vanished.size() <= 64) {
        struct Pair {
            qsizetype created, vanished;
            double score;
        };
        QList<Pair> pairs;
        QList<QHash<QString, int>> bases;
        QList<int> baseTotals;
        for (const SyncJournal::Entity &gone : std::as_const(vanished)) {
            int total = 0;
            bases.append(lineBag(journal.readBase(gone.entityID), &total)); // no base yet: never matched
            baseTotals.append(total);
        }
        for (qsizetype c = 0; c < created.size(); ++c) {
            Note note;
            if (!Vault::read(vault.absolute(changes.at(created.at(c)).path), note))
                continue;
            int total = 0;
            const QHash<QString, int> bag = lineBag(note.text, &total);
            double best = 0;
            double second = 0;
            qsizetype bestVanished = -1;
            for (qsizetype v = 0; v < vanished.size(); ++v) {
                if (total == 0 || baseTotals.at(v) == 0)
                    continue;
                int common = 0;
                for (auto line = bag.cbegin(); line != bag.cend(); ++line)
                    common += std::min(line.value(), bases.at(v).value(line.key()));
                const double score = 2.0 * common / (total + baseTotals.at(v));
                if (score > best) {
                    second = best;
                    best = score;
                    bestVanished = v;
                } else if (score > second) {
                    second = score;
                }
            }
            if (bestVanished >= 0 && best >= 0.5 && best > second)
                pairs.append({c, bestVanished, best});
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.score > b.score; });
        QSet<qsizetype> claimed;
        for (const Pair &pair : std::as_const(pairs)) {
            if (claimed.contains(pair.vanished))
                continue;
            claimed.insert(pair.vanished);
            const SyncJournal::Entity &gone = vanished.at(pair.vanished);
            Change &change = changes[created.at(pair.created)];
            change.kind = Change::Renamed;
            change.oldPath = gone.path;
            change.entityID = gone.entityID;
            known.remove(gone.path);
        }
    }

    for (const SyncJournal::Entity &gone : known) {
        Change change;
        change.kind = Change::Deleted;
        change.entityID = gone.entityID;
        change.path = gone.path;
        change.entityType = gone.entityType;
        changes.append(change);
    }

    return changes;
}

} // namespace noteAppC
