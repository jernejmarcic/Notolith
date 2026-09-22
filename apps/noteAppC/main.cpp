#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Fusion, so the window looks the same whatever desktop theme is set;
    // everything visible on top of it comes from Theme's tokens.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOTEAPPC_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("A Linux-native Obsidian alternative in C++"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("vault"),
                                 QStringLiteral("Vault directory to open."));
    parser.process(app);

    noteAppC::MainWindow window;
    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openVault(args.first());
    window.show();

    return app.exec();
}
