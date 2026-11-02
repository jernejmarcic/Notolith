// SPDX-License-Identifier: GPL-3.0-or-later
// The static counterpart of FakeModule.cpp (docs/PLATFORMS.md, "Mobile"): linked into test_modules as a Qt
// static plugin when NOTEAPPC_STATIC_MODULES is on, found through QPluginLoader::staticPlugins() instead of a
// directory. Its class name differs from FakeModule's: moc output, vtables and staticMetaObject of two classes
// with one name would collide in one process. The constructor leaves the same mark FakeModule's initializer does,
// so a test can prove the scan did not construct it.
#include <QFile>
#include <QObject>
#include <QtPlugin>

#include "ModuleHost.h"
#include "NotolithModule.h"

class FakeStaticModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    FakeStaticModule()
    {
        const QByteArray mark = qgetenv("NOTEAPPC_FAKE_MODULE_MARK");
        if (!mark.isEmpty()) {
            QFile file(QString::fromLocal8Bit(mark));
            if (file.open(QIODevice::WriteOnly | QIODevice::Append))
                file.write("constructed\n");
        }
    }
    void attach(noteAppC::ModuleHost &host) override
    {
        host.registerView({QStringLiteral(FAKE_SUFFIX)}, [](const QString &, QWidget *) -> noteAppC::ModuleView * { return nullptr; });
        host.registerCommand(QStringLiteral(FAKE_ID ":hello"), QStringLiteral("Fake: say hello"), [] {});
    }
};

#include "FakeStaticModule.moc"
