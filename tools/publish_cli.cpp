// Publishes a vault - or its `publish: true` part - as a static website.
//
//   noteAppC-publish <vault> <outdir> [--only-marked] [--home <note>]
//   [--site-name <name>] [--theme dark|light|both]
//
// Indexes the vault the way tools/index_cli.cpp does (Indexer, wait for
// finished()), then hands the open Index straight to Publisher: the index is
// what resolve()/backlinks() need, and re-scanning it inside Publisher would
// just be this same work done twice.

// A real QApplication, not QCoreApplication: MathRenderer (MicroTeX) loads
// fonts through QFontDatabase and CodeHighlighter/HtmlRenderer build QFonts
// and QImages, all of which need a GUI platform plugin behind them - the
// same reason tools/bench/Bench.cpp is a QApplication despite never showing
// a window. Run with QT_QPA_PLATFORM=offscreen, the way every other
// headless tool and test in this project does.
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTextStream>

#include "Index.h"
#include "Indexer.h"
#include "Publisher.h"
#include "Vault.h"

using namespace noteAppC;
using namespace noteAppC::publish;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Export a vault as a static website."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("vault"), QStringLiteral("Vault directory."));
    parser.addPositionalArgument(QStringLiteral("outdir"), QStringLiteral("Where to write the site."));
    QCommandLineOption onlyMarked(QStringLiteral("only-marked"),
                                  QStringLiteral("Publish only notes with publish: true."));
    QCommandLineOption home(QStringLiteral("home"),
                            QStringLiteral("Vault-relative note that becomes index.html."),
                            QStringLiteral("note"));
    QCommandLineOption siteName(QStringLiteral("site-name"), QStringLiteral("Site name shown in the header."),
                                QStringLiteral("name"), QStringLiteral("Notes"));
    QCommandLineOption theme(QStringLiteral("theme"),
                             QStringLiteral("dark, light or both (default: both, via prefers-color-scheme)."),
                             QStringLiteral("mode"), QStringLiteral("both"));
    parser.addOption(onlyMarked);
    parser.addOption(home);
    parser.addOption(siteName);
    parser.addOption(theme);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.size() < 2)
        parser.showHelp(2);

    const Vault vault(args.at(0));
    if (!vault.isValid()) {
        qWarning("%s is not a directory", qPrintable(args.at(0)));
        return 2;
    }

    QTextStream out(stdout);
    QElapsedTimer clock;
    clock.start();

    // Index synchronously: a CLI run has nothing else to do meanwhile.
    Index index;
    Indexer indexer(&index);
    QEventLoop loop;
    int indexedCount = 0;
    qint64 indexMs = 0;
    QObject::connect(&indexer, &Indexer::finished, &loop,
                     [&](int indexed, qint64 ms) {
                         indexedCount = indexed;
                         indexMs = ms;
                         loop.quit();
                     });
    indexer.setVault(vault);
    loop.exec();
    out << "indexed  " << indexedCount << " notes in " << indexMs << " ms\n";

    Publisher publisher(vault, index);
    Options options;
    options.outputDir = args.at(1);
    options.onlyMarked = parser.isSet(onlyMarked);
    options.homeNote = parser.value(home);
    options.siteName = parser.value(siteName);
    const QString themeValue = parser.value(theme).toLower();
    if (themeValue == QLatin1String("dark"))
        options.theme = Options::ThemeMode::Dark;
    else if (themeValue == QLatin1String("light"))
        options.theme = Options::ThemeMode::Light;
    else
        options.theme = Options::ThemeMode::Both;

    QElapsedTimer publishClock;
    publishClock.start();
    QString error;
    const bool ok = publisher.publish(
        options,
        [&out](int done, int total) {
            if (total > 0 && (done == total || done % 500 == 0))
                out << "  " << done << "/" << total << " notes\n";
        },
        &error);
    const qint64 publishMs = publishClock.elapsed();

    if (!ok) {
        qWarning("publish failed: %s", qPrintable(error));
        return 1;
    }

    const Counts counts = publisher.counts();
    out << "published " << options.outputDir << " in " << publishMs << " ms\n"
        << "pages     " << counts.pages << "\n"
        << "attachments " << counts.attachments << "\n"
        << "skipped   " << counts.skipped << " (not published)\n"
        << "unchanged " << counts.unchanged << " (incremental republish)\n"
        << "removed   " << counts.removed << " (no longer published)\n"
        << "wall      " << clock.elapsed() << " ms total\n";
    out.flush();
    return 0;
}
