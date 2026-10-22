// The test suite's module: discovered by metadata, loaded only on first use.
// Its static initializer leaves a mark, so a test can prove that reading the
// manifest did not load the library (spec section 4, "Gate").
#include <QFile>
#include <QLabel>
#include <QVBoxLayout>
#include <QtPlugin>

#include "ModuleHost.h"
#include "NotolithModule.h"

namespace {
const bool marked = [] {
    const QByteArray mark = qgetenv("NOTEAPPC_FAKE_MODULE_MARK");
    if (!mark.isEmpty()) {
        QFile file(QString::fromLocal8Bit(mark));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write("loaded\n");
    }
    return true;
}();

class FakeView : public noteAppC::ModuleView {
public:
    explicit FakeView(QWidget *parent) : ModuleView(parent)
    {
        setObjectName(QStringLiteral("fakeView"));
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(m_label = new QLabel(this));
    }
    bool load(const QString &path, QString *) override { m_path = path; m_label->setText(path); return true; }
    bool save(QString *) override { return true; }
    bool isModified() const override { return false; }
    void setTheme(const noteAppC::Theme &) override {}
private:
    QLabel *m_label = nullptr;
    QString m_path;
};
} // namespace

class FakeModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    void attach(noteAppC::ModuleHost &host) override
    {
        host.registerView({QStringLiteral(FAKE_SUFFIX)},
                          [](const QString &, QWidget *parent) -> noteAppC::ModuleView * { return new FakeView(parent); });
        const auto status = host.showStatus;
        const auto folder = host.currentFolder;
        host.registerCommand(QStringLiteral(FAKE_ID ":hello"), QStringLiteral("Fake: say hello"),
                             [status] { if (status) status(QStringLiteral("hello from the fake module"), 2000); });
        host.registerCommand(QStringLiteral(FAKE_ID ":where"), QStringLiteral("Fake: show the current folder"),
                             [status, folder] { if (status && folder) status(folder(), 2000); });
        host.registerSetting({QStringLiteral("fake.greeting"), true, QStringLiteral("Fake: greet"),
                              QStringLiteral("A toggle the fake module registered."), {}, 0, 0});
        // A decimal row (spec section 4, settings rows): a double spin box, not an integer one.
        host.registerSetting({QStringLiteral("fake.width"), 2.0, QStringLiteral("Fake: width"),
                              QStringLiteral("A decimal the fake module registered."), {}, 0.5, 40.0, 0.5, 1});
    }
};

#include "FakeModule.moc"
