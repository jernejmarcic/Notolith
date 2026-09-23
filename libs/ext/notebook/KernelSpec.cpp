#include "KernelSpec.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

namespace noteAppC::notebook {

QStringList kernelSpecRoots()
{
    QStringList roots;

    const QString jupyterPath = qEnvironmentVariable("JUPYTER_PATH");
    if (!jupyterPath.isEmpty()) {
        for (const QString &entry : jupyterPath.split(QLatin1Char(':'), Qt::SkipEmptyParts))
            roots << entry + QStringLiteral("/kernels");
    }

    QString dataDir = qEnvironmentVariable("JUPYTER_DATA_DIR");
    if (dataDir.isEmpty()) {
        QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
        if (xdgData.isEmpty())
            xdgData = QDir::homePath() + QStringLiteral("/.local/share");
        dataDir = xdgData + QStringLiteral("/jupyter");
    }
    roots << dataDir + QStringLiteral("/kernels");

    // The system locations jupyter_core.paths hard-codes for Linux, plus the
    // running interpreter's own prefix (a venv's ipykernel installs its
    // kernelspec there) - skipped here since this app never runs Python and
    // so has no interpreter prefix of its own to add.
    roots << QStringLiteral("/usr/local/share/jupyter/kernels")
          << QStringLiteral("/usr/share/jupyter/kernels");
    return roots;
}

namespace {

KernelSpec readKernelJSON(const QString &path, const QString &name)
{
    KernelSpec spec;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return spec;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.isEmpty())
        return spec;

    spec.name = name;
    spec.displayName = object.value(QStringLiteral("display_name")).toString(name);
    spec.language = object.value(QStringLiteral("language")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("argv")).toArray())
        spec.argv << value.toString();
    const QJsonObject metadata = object.value(QStringLiteral("metadata")).toObject();
    spec.interruptMode =
        object.value(QStringLiteral("interrupt_mode")).toString(QStringLiteral("signal"));
    Q_UNUSED(metadata); // read for completeness; nothing here needs it yet
    spec.kernelJSONpath = path;
    return spec;
}

} // namespace

QList<KernelSpec> discoverKernelSpecs()
{
    QList<KernelSpec> specs;
    QSet<QString> seen;
    for (const QString &root : kernelSpecRoots()) {
        QDir dir(root);
        if (!dir.exists())
            continue;
        const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &name : names) {
            if (seen.contains(name))
                continue; // a more specific root already won this name
            const QString kernelJSON = dir.filePath(name + QStringLiteral("/kernel.json"));
            if (!QFile::exists(kernelJSON))
                continue;
            const KernelSpec spec = readKernelJSON(kernelJSON, name);
            if (spec.name.isEmpty() || spec.argv.isEmpty())
                continue; // unreadable or malformed: skipped, not fatal to discovery
            seen.insert(name);
            specs.append(spec);
        }
    }
    return specs;
}

QHash<QString, KernelSpec> kernelSpecsByLanguage()
{
    QHash<QString, KernelSpec> byLanguage;
    for (const KernelSpec &spec : discoverKernelSpecs()) {
        const QString language = spec.language.trimmed().toLower();
        if (language.isEmpty() || byLanguage.contains(language))
            continue;
        byLanguage.insert(language, spec);
    }
    return byLanguage;
}

QStringList substituteConnectionFile(const QStringList &argv, const QString &path)
{
    QStringList result = argv;
    for (QString &arg : result) {
        if (arg == QLatin1String("{connection_file}"))
            arg = path;
    }
    return result;
}

} // namespace noteAppC::notebook
