// noteAppC-sync-server: the reference server for docs/SYNC.md. Stores and
// relays ciphertext only - see ServerStore and SyncServer for what that
// means concretely, and docs/SYNC.md "What the server stores and learns"
// for what it does not.
//
//   noteAppC-sync-server --listen 127.0.0.1:<port> --data <dir> [--cert <file> --key <file>]
//
// Port 0 lets the OS pick one (printed on stdout as "listening on <port>"
// once bound) - what tests/test_sync.cpp uses so runs never collide.
#include <cstdio>

#include <QCoreApplication>
#include <QHostAddress>
#include <QStringList>

#include <sodium.h>

#include "ServerStore.h"
#include "SyncServer.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (sodium_init() < 0) {
        std::fprintf(stderr, "libsodium failed to initialise\n");
        return 1;
    }

    QString listenArg = QStringLiteral("127.0.0.1:0");
    QString dataDir;
    QString certFile;
    QString keyFile;
    const QStringList args = app.arguments();
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--listen") && i + 1 < args.size())
            listenArg = args.at(++i);
        else if (args.at(i) == QStringLiteral("--data") && i + 1 < args.size())
            dataDir = args.at(++i);
        else if (args.at(i) == QStringLiteral("--cert") && i + 1 < args.size())
            certFile = args.at(++i);
        else if (args.at(i) == QStringLiteral("--key") && i + 1 < args.size())
            keyFile = args.at(++i);
    }
    if (dataDir.isEmpty()) {
        std::fprintf(stderr, "usage: noteAppC-sync-server --listen host:port --data <dir> "
                             "[--cert <file> --key <file>]\n");
        return 1;
    }

    const qsizetype colon = listenArg.lastIndexOf(QLatin1Char(':'));
    if (colon < 0) {
        std::fprintf(stderr, "--listen wants host:port\n");
        return 1;
    }
    const QString host = listenArg.left(colon);
    bool portOk = false;
    const quint16 port = listenArg.mid(colon + 1).toUShort(&portOk);
    if (!portOk) {
        std::fprintf(stderr, "--listen wants host:port\n");
        return 1;
    }

    noteAppC::ServerStore store(dataDir);
    if (!store.isOpen()) {
        std::fprintf(stderr, "could not open the data store: %s\n", qPrintable(store.error()));
        return 1;
    }
    noteAppC::SyncServer server(&store);
    if (!server.listen(host, port, certFile, keyFile)) {
        std::fprintf(stderr, "could not listen on %s\n", qPrintable(listenArg));
        return 1;
    }
    std::printf("listening on %u\n", server.actualPort());
    std::fflush(stdout);
    return app.exec();
}
