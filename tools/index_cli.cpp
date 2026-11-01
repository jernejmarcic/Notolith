// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Indexes a vault and prints what the cache holds: the quickest way to run
// the indexer over a real vault.
//
//   noteAppC_index [--fresh] <vault> [query]

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include "Index.h"
#include "Indexer.h"
#include "Vault.h"

using namespace noteAppC;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("vault"), QStringLiteral("Vault directory."));
    parser.addPositionalArgument(QStringLiteral("query"),
                                 QStringLiteral("Optional name to resolve and show backlinks for."));
    QCommandLineOption fresh(QStringLiteral("fresh"),
                             QStringLiteral("Delete the cache first, to time a cold build."));
    parser.addOption(fresh);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(2);

    const Vault vault(args.first());
    if (!vault.isValid()) {
        qWarning("%s is not a directory", qPrintable(args.first()));
        return 2;
    }

    QTextStream out(stdout);
    if (parser.isSet(fresh))
        QFile::remove(Index::cachePathFor(vault.root()));

    Index index;
    Indexer indexer(&index);
    QElapsedTimer clock;

    QObject::connect(&indexer, &Indexer::finished, &app, [&](int indexed, qint64 milliseconds) {
        out << "vault    " << vault.root() << "\n"
            << "cache    " << index.databasePath() << "\n"
            << "indexed  " << indexed << " notes in " << milliseconds << " ms"
            << " (wall " << clock.elapsed() << " ms)\n"
            << "notes    " << index.noteCount() << "\n"
            << "tags     " << index.allTags().size() << "\n"
            << "names    " << index.linkTargets().size() << "\n";

        if (args.size() > 1) {
            const QString target = args.at(1);
            const QString resolved = index.resolve(target);
            out << "\nresolve  " << target << " -> "
                << (resolved.isEmpty() ? QStringLiteral("(unresolved)") : resolved) << "\n";
            if (!resolved.isEmpty()) {
                const auto backlinks = index.backlinks(vault.absolute(resolved));
                out << "backlinks " << backlinks.size() << "\n";
                for (int i = 0; i < qMin<qsizetype>(10, backlinks.size()); ++i)
                    out << "  " << backlinks.at(i).fromPath << "\n";
            }
        }
        out.flush();
        app.quit();
    });

    clock.start();
    indexer.setVault(vault);
    return app.exec();
}
