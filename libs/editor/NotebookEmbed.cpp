/// Runnable code-file embeds: `![[script.py]]`
/// gets the same run control and output NotebookBlock.cpp already gives a
/// fenced block - notebookEligible, appendNotebookOutput, the paint and
/// click paths in MarkdownEditor.cpp/BlockRenderers.cpp all key off those
/// same fields regardless of which one set them. The only thing an embed
/// needs of its own is a language: not a fence's info string, but the
/// file's extension, the way a kernel.json's "language" plus the handful of
/// extensions a real Jupyter kernel install actually registers would.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QFileInfo>
#include <QHash>

namespace noteAppC {

namespace {

QString languageForExtension(const QString &suffix)
{
    static const QHash<QString, QString> languages = {
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("ipy"), QStringLiteral("python")},
        {QStringLiteral("jl"), QStringLiteral("julia")},
        {QStringLiteral("r"), QStringLiteral("r")},
        {QStringLiteral("rb"), QStringLiteral("ruby")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("ts"), QStringLiteral("typescript")},
        {QStringLiteral("sh"), QStringLiteral("bash")},
        {QStringLiteral("bash"), QStringLiteral("bash")},
        {QStringLiteral("rs"), QStringLiteral("rust")},
        {QStringLiteral("go"), QStringLiteral("go")},
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("cpp"), QStringLiteral("cpp")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("hs"), QStringLiteral("haskell")},
        {QStringLiteral("java"), QStringLiteral("java")},
        {QStringLiteral("lua"), QStringLiteral("lua")},
        {QStringLiteral("pl"), QStringLiteral("perl")},
    };
    return languages.value(suffix.trimmed().toLower());
}

} // namespace

void MarkdownEditor::layoutRunnableEmbed(BlockView &view, const QString &path, const QString &body)
{
    if (!d->m_features.notebooks || !d->m_notebookRunner)
        return;
    const QString language = languageForExtension(QFileInfo(path).suffix());
    if (language.isEmpty())
        return;

    view.notebookEligible = true;
    // The kernel's working directory for this run: the embedded file's own
    // folder, bound read-only in the bubblewrap tier (NotebookRunOptions,
    // MainWindow's own adapter turns it into extraReadOnlyBinds) - so a
    // script that opens a sibling file still can. Never `#| sandbox: host`:
    // that option only exists on a fence's own `#|` lines, which a file on
    // disk does not have.
    appendNotebookOutput(view, language, body, QStringLiteral("e") + path, QFileInfo(path).absolutePath());
}

} // namespace noteAppC
