#include "SyncScanner.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

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
    QDirIterator it(vault.root(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
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
