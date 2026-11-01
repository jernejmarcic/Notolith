// The pandoc core plugin's side of the window: its commands, the dialogs and the two jobs (export a note,
// import a file). What the Markdown becomes, the argument lists and the process are libs/ext/pandoc's; this
// file only asks the vault and the index what they know and shows the outcome.

#include "MainWindow.h"

#include "FormatColours.h"
#include "Frontmatter.h"
#include "MermaidCache.h"
#include "Pandoc.h"
#include "PandocPrepare.h"
#include "PandocRunner.h"
#include "SettingsPage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QSvgRenderer>
#include "SafeSvg.h"
#include <QTemporaryDir>
#include <QTimer>

namespace noteAppC {

void MainWindow::buildPandocActions()
{
    const auto add = [this](const QString &id, const QString &name, auto slot) {
        auto *action = new QAction(name, this);
        action->setObjectName(id);
        action->setData(QString()); // no built-in keys; hotkeys.json can bind one
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
        m_commands << action;
    };
    for (const pandoc::Format &format : pandoc::exportFormats()) {
        add(QStringLiteral("pandoc:export-") + format.id, tr("Pandoc: Export as %1").arg(format.label),
            [this, id = format.id] {
                if (m_active->notePath.isEmpty() || m_active->view)
                    statusBar()->showMessage(tr("Open a note to export it"), 3000);
                else
                    exportWithPandoc(m_active->notePath, id);
            });
    }
    add(QStringLiteral("pandoc:export-other"), tr("Pandoc: Export as another format..."), [this] {
        if (m_active->notePath.isEmpty() || m_active->view)
            statusBar()->showMessage(tr("Open a note to export it"), 3000);
        else
            exportWithPandoc(m_active->notePath, QStringLiteral("other"));
    });
    add(QStringLiteral("pandoc:import"), tr("Pandoc: Import a file as a note..."), [this] { importWithPandocChosen(); });
}

void MainWindow::refreshPandocCommands()
{
    if (!featureOn("pandoc"))
        return; // off: nothing is looked up
    const bool found = !pandoc::locate(m_settings.text(QStringLiteral("pandoc.path"))).isEmpty();
    for (QAction *action : std::as_const(m_commands)) {
        if (!action->objectName().startsWith(QLatin1String("pandoc:")))
            continue;
        // Drawn disabled (the palette and menus read "dimmed") but enabled: using it explains and points to Settings.
        action->setProperty("dimmed", !found);
        action->setToolTip(found ? QString() : tr("pandoc was not found: click to see how to get it"));
    }
}

QString MainWindow::pandocProgram()
{
    const QString program = pandoc::locate(m_settings.text(QStringLiteral("pandoc.path")));
    if (program.isEmpty())
        showPandocMissing();
    return program;
}

void MainWindow::showPandocMissing()
{
    if (auto *open = findChild<QMessageBox *>(QStringLiteral("pandocMissingDialog"))) {
        open->raise();
        return;
    }
    QWidget *parent = m_settingsWindow->isVisible() ? static_cast<QWidget *>(m_settingsWindow) : this;
    auto *box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("pandocMissingDialog"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Information);
    box->setWindowTitle(tr("Pandoc not found"));
    const QString configured = m_settings.text(QStringLiteral("pandoc.path")).trimmed();
    box->setText(configured.isEmpty()
                     ? tr("This depends on pandoc, which was not found on PATH.")
                     : tr("This depends on pandoc, which was not found on PATH or at the configured path (%1).")
                           .arg(configured));
    box->setInformativeText(tr("Install it with your package manager - on Arch Linux: pacman -S pandoc; elsewhere see "
                               "https://pandoc.org/installing.html - or, if it is installed somewhere else, set its "
                               "path in Settings."));
    QPushButton *set = box->addButton(tr("Set the pandoc path in Settings..."), QMessageBox::ActionRole);
    set->setObjectName(QStringLiteral("pandocSetPath"));
    box->addButton(QMessageBox::Close);
    connect(set, &QPushButton::clicked, this, [this] {
        openSettings();
        m_settingsPage->showPandocSettings();
    });
    box->open();
}

void MainWindow::runPandoc(const QString &program, const QStringList &arguments, const QByteArray &input,
                           const QString &workingDirectory, const QString &title, std::function<void()> done,
                           const QStringList &readOnlyBinds)
{
    // pandoc runs only in the sandbox: the note and the file it imports are anyone's, and a document can
    // name any file to be read into the result, or any URL to be fetched. It sees /usr, the paths it was
    // given to read, and `workingDirectory` to write (a scratch folder of ours: the result is copied from
    // there); no network, no home, no other drive. Without bubblewrap it does not run.
    const sandbox::Command sandboxed = sandbox::bubblewrapCommand(workingDirectory, program, arguments, {}, readOnlyBinds);
    if (sandboxed.program.isEmpty()) {
        QMessageBox::warning(this, title, tr("pandoc runs only inside bubblewrap's sandbox (with libseccomp); install bubblewrap."));
        return;
    }
    sandbox::Limits limits; // not RLIMIT_AS: pandoc's runtime reserves far more address space than it uses
    limits.cpuSeconds = 150;
    limits.data = 4ULL * 1024 * 1024 * 1024;
    limits.fileSize = 512ULL * 1024 * 1024;
    limits.openFiles = 1024;
    if (!m_pandocRunner)
        m_pandocRunner = new pandoc::Runner(this);
    auto *progress = new QProgressDialog(tr("Running pandoc..."), tr("Cancel"), 0, 0, this);
    progress->setObjectName(QStringLiteral("pandocProgress"));
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(400); // a quick run never shows it
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, m_pandocRunner, &pandoc::Runner::cancel);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_pandocRunner, &pandoc::Runner::finished, this,
                          [this, connection, progress, title, done = std::move(done)](bool ok, const QString &message) {
        disconnect(*connection);
        progress->close();
        progress->deleteLater();
        const bool cancelled = m_pandocRunner->wasCancelled();
        if (ok) {
            done();
        } else if (!cancelled) {
            QMessageBox box(QMessageBox::Warning, title, tr("pandoc could not finish."), QMessageBox::Close, this);
            box.setDetailedText(message);
            box.setInformativeText(message.section(QLatin1Char('\n'), 0, 0));
            box.exec();
        }
        // The next file of a drop, failed or not; a cancel stops the rest as well.
        if (cancelled)
            m_pandocQueue.clear();
        if (!m_pandocQueue.isEmpty()) {
            QTimer::singleShot(0, this, [this] {
                if (m_pandocQueue.isEmpty() || (m_pandocRunner && m_pandocRunner->running()))
                    return;
                if (!featureOn("pandoc")) { // switched off meanwhile: nothing more is started
                    m_pandocQueue.clear();
                    return;
                }
                const auto next = m_pandocQueue.takeFirst();
                importWithPandoc(next.first, next.second);
            });
        }
    });
    m_pandocRunner->start(sandboxed.program, sandboxed.arguments, input, workingDirectory, 120000, sandboxed.filterFD, limits);
}

namespace {

/// A warning that does not hold the window: a test, or a person with something else to do, goes on.
void showNotice(QWidget *parent, const QString &objectName, const QString &title, const QString &text)
{
    if (auto *old = parent->findChild<QMessageBox *>(objectName))
        old->close();
    auto *box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Close, parent);
    box->setObjectName(objectName);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

/// A diagram's cached SVG as a PNG in `folder`, wide enough to stay sharp on paper, on white.
QString rasterised(const QByteArray &svg, const QString &folder)
{
    QSvgRenderer renderer(safeSvg(svg));
    if (!renderer.isValid() || renderer.defaultSize().isEmpty())
        return {};
    const QSize natural = renderer.defaultSize();
    const qreal scale = qMin(2.0, 1600.0 / natural.width());
    QImage image(QSize(natural.width() * scale, natural.height() * scale).expandedTo(QSize(1, 1)), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    renderer.render(&painter);
    painter.end();
    const QString path = QDir(folder).filePath(
        QString::fromLatin1(QCryptographicHash::hash(svg, QCryptographicHash::Sha1).toHex()) + QStringLiteral(".png"));
    return image.save(path) ? path : QString();
}

} // namespace

void MainWindow::exportWithPandoc(const QString &notePath, const QString &formatID, const QString &outputPath)
{
    if (notePath.isEmpty())
        return;
    const QString program = pandocProgram();
    if (program.isEmpty())
        return;
    if (m_pandocRunner && m_pandocRunner->running()) {
        statusBar()->showMessage(tr("pandoc is already running"), 4000);
        return;
    }

    QString writer, extension, label;
    QStringList formatArguments;
    bool takesReferenceDoc = false;
    if (formatID.startsWith(QLatin1String("other"))) {
        bool ok = formatID.size() > 5;
        QString name = formatID.mid(6);
        if (!ok) {
            name = QInputDialog::getText(this, tr("Export with pandoc"),
                                         tr("Pandoc output format (pandoc --list-output-formats), e.g. rst, org, "
                                            "asciidoc, typst or markdown+smart:"),
                                         QLineEdit::Normal, QStringLiteral("rst"), &ok).trimmed();
        }
        if (!ok || name.isEmpty())
            return;
        if (!pandoc::validFormatName(name)) {
            QMessageBox::warning(this, tr("Export with pandoc"),
                                 tr("\"%1\" is not a pandoc format name: letters, digits, _ + and - only.").arg(name));
            return;
        }
        writer = name;
        extension = pandoc::extensionForWriter(name);
        label = name;
    } else {
        const auto formats = pandoc::exportFormats();
        const auto found = std::find_if(formats.cbegin(), formats.cend(),
                                        [&formatID](const pandoc::Format &format) { return format.id == formatID; });
        if (found == formats.cend())
            return;
        writer = found->writer;
        extension = found->extension;
        label = found->label;
        formatArguments = found->extra;
        takesReferenceDoc = found->referenceDoc;
    }

    const QFileInfo note(notePath);
    const QString target = !outputPath.isEmpty()
        ? outputPath
        : QFileDialog::getSaveFileName(
              this, tr("Export with pandoc"),
              note.dir().filePath(pandoc::defaultExportName(note.completeBaseName(), writer, extension)),
              QStringLiteral("%1 (*.%2);;%3").arg(label, extension, tr("All files (*)")));
    if (target.isEmpty())
        return;
    // Never over a note: a Markdown writer's file is the note's own name's, and its wikilinks, embeds and
    // comments would be gone from the note on disk while the editor still held them.
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() && (targetInfo.canonicalFilePath() == note.canonicalFilePath()
                                || (m_vault.isValid() && m_vault.contains(target) && Vault::isNote(target)))) {
        showNotice(this, QStringLiteral("pandocRefusedDialog"), tr("Export with pandoc"),
                   tr("%1 is a note, and exporting would overwrite it. Choose another name.").arg(targetInfo.fileName()));
        return;
    }

    // What is on screen if the note is open, else what is on disk.
    QString text;
    bool haveText = false;
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == notePath && !leaf->view) {
            text = leaf->editor->noteText();
            haveText = true;
            break;
        }
    }
    if (!haveText) {
        Note onDisk;
        QString error;
        if (!Vault::read(notePath, onDisk, &error)) {
            QMessageBox::warning(this, tr("Export with pandoc"), tr("Could not read the note: %1").arg(error));
            return;
        }
        text = onDisk.text;
    }

    pandoc::PrepareContext context;
    context.vaultRoot = m_vault.isValid() ? m_vault.root() : note.absolutePath(); // a stray note: its own folder
    context.resolve = [this](const QString &name, const QString &from) -> QString {
        if (m_vault.isValid()) {
            const QString resolved = m_index.resolve(name, from);
            if (!resolved.isEmpty())
                return m_vault.absolute(resolved);
        }
        // The index has notes by name; a file is found where the editor looks for it: beside the note, at
        // the vault root, in the attachment folder - and a bare name is a note's.
        const QString attachments = m_vault.isValid()
            ? m_vault.attachmentFolderFor(m_settings.text(QStringLiteral("files.attachmentFolder")), from)
            : QString();
        QStringList folders = {QFileInfo(from).absolutePath()};
        if (m_vault.isValid())
            folders << m_vault.root() << attachments;
        for (const QString &candidate : {name, QFileInfo(name).suffix().isEmpty() ? name + QStringLiteral(".md") : QString()}) {
            if (candidate.isEmpty())
                continue;
            for (const QString &folder : std::as_const(folders)) {
                if (QFileInfo(QDir(folder).filePath(candidate)).isFile())
                    return QDir(folder).filePath(candidate);
            }
        }
        return {};
    };
    context.readNote = [](const QString &path) -> std::optional<QString> {
        Note embedded;
        if (!Vault::read(path, embedded))
            return std::nullopt;
        return embedded.text;
    };
    // The diagrams the editor has already drawn, never drawn here: as pictures in the colours of paper.
    auto images = std::make_shared<QTemporaryDir>();
    Theme paper = Theme::builtin(Theme::Mode::Light);
    applyUserColours(paper, [this](const QString &key) { return m_settings.text(key); });
    context.diagramImage = [images, paper](const QString &language, const QString &source) -> QString {
        for (const DiagramKind kind : {DiagramKind::Mermaid, DiagramKind::PlantUML, DiagramKind::TikZ, DiagramKind::Graphviz}) {
            if (!diagramLanguages(kind).contains(language))
                continue;
            const QByteArray svg = diagramCachedSVG(kind, source, paper);
            return svg.isEmpty() || !images->isValid() ? QString() : rasterised(svg, images->path());
        }
        return {};
    };
    const pandoc::PrepareResult prepared = pandoc::prepareMarkdown(text, notePath, context);

    // pandoc writes into a scratch folder it alone can write to, and the result is copied to `target` here.
    auto scratch = std::make_shared<QTemporaryDir>();
    if (!scratch->isValid()) {
        statusBar()->showMessage(tr("Could not make a scratch folder for pandoc"), 5000);
        return;
    }
    const QString produced = scratch->filePath(QStringLiteral("export.") + extension);
    pandoc::ExportRequest request;
    request.writer = writer;
    request.formatArguments = formatArguments;
    request.outputPath = produced;
    request.resourcePaths = {note.absolutePath()};
    if (m_vault.isValid()) {
        request.resourcePaths << m_vault.root()
                              << m_vault.attachmentFolderFor(m_settings.text(QStringLiteral("files.attachmentFolder")), notePath);
    }
    // What it may read: those folders, inside the vault and never the whole home directory or the disk.
    QStringList readable;
    for (const QString &folder : std::as_const(request.resourcePaths)) {
        const QString real = QFileInfo(folder).canonicalFilePath();
        if (real.isEmpty() || real == QLatin1String("/") || real == QDir::home().canonicalPath() || readable.contains(real))
            continue;
        readable << real;
    }
    const QString reference = m_settings.text(QStringLiteral("pandoc.referenceDoc")).trimmed();
    if (takesReferenceDoc && !reference.isEmpty() && QFileInfo(reference).isFile()) {
        request.referenceDoc = QFileInfo(reference).canonicalFilePath();
        readable << request.referenceDoc;
    }
    // A note with no title of its own is titled by its name (EPUB and LaTeX want one).
    const Range frontmatter = Frontmatter::find(prepared.markdown);
    static const QRegularExpression hasTitle(QStringLiteral("^title\\s*:"), QRegularExpression::MultilineOption);
    if (!frontmatter.isValid() || !hasTitle.match(prepared.markdown.left(frontmatter.end)).hasMatch())
        request.title = note.completeBaseName();
    request.extraArguments = pandoc::splitArguments(m_settings.text(QStringLiteral("pandoc.extraArguments")));

    const int unresolved = prepared.unresolved;
    runPandoc(program, pandoc::exportArguments(request), prepared.markdown.toUtf8(), scratch->path(),
              tr("Export with pandoc"), [this, target, produced, scratch, unresolved, images] {
                  // The scratch directory is the sandbox's to write: a link there to a user's file is not pandoc's output.
                  const QFileInfo made(produced);
                  if (made.isSymLink() || !made.isFile()) {
                      showNotice(this, QStringLiteral("pandocNotAFileDialog"), tr("Export with pandoc"),
                                 tr("pandoc did not write a file, so nothing was exported."));
                      return;
                  }
                  QFile::remove(target); // the save dialog has asked about replacing it
                  if (!QFile::copy(produced, target)) {
                      QMessageBox::warning(this, tr("Export with pandoc"), tr("Could not write %1").arg(target));
                      return;
                  }
                  QString message = tr("Exported %1").arg(QFileInfo(target).fileName());
                  if (unresolved > 0)
                      message += tr(" (%n link(s) or embed(s) named nothing in the vault)", nullptr, unresolved);
                  statusBar()->showMessage(message, 6000);
              },
              readable);
}

void MainWindow::importWithPandocChosen()
{
    if (!m_vault.isValid()) {
        statusBar()->showMessage(tr("Importing makes a note in a vault"), 4000);
        return;
    }
    if (pandocProgram().isEmpty()) // before asking for a file it could not be used on
        return;
    QStringList patterns;
    for (const QString &suffix : pandoc::importSuffixes())
        patterns << QStringLiteral("*.") + suffix;
    const QString file = QFileDialog::getOpenFileName(this, tr("Import with pandoc"), QDir::homePath(),
                                                      tr("Documents (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
    if (!file.isEmpty())
        importWithPandoc(file, newNoteFolder());
}

void MainWindow::importWithPandoc(const QString &file, const QString &folderRelative)
{
    if (!m_vault.isValid()) {
        statusBar()->showMessage(tr("Importing makes a note in a vault"), 4000);
        return;
    }
    const QFileInfo input(file);
    const QString reader = pandoc::readerForSuffix(input.suffix());
    if (!input.isFile() || reader.isEmpty()) {
        statusBar()->showMessage(tr("pandoc is not offered for %1 here").arg(input.fileName()), 5000);
        return;
    }
    const QString program = pandocProgram();
    if (program.isEmpty())
        return;
    if (m_pandocRunner && m_pandocRunner->running()) {
        // Several files dropped at once: each is imported when the one before has finished.
        m_pandocQueue.append({file, folderRelative});
        statusBar()->showMessage(tr("%1 waits for pandoc (%n file(s) queued)", nullptr, int(m_pandocQueue.size()))
                                     .arg(input.fileName()), 4000);
        return;
    }
    // Scratch space for the Markdown and the media pandoc takes out of the file; nothing reaches the vault
    // until pandoc has finished, so a failed or cancelled import leaves no note behind.
    auto scratch = std::make_shared<QTemporaryDir>();
    if (!scratch->isValid()) {
        statusBar()->showMessage(tr("Could not make a scratch folder for pandoc"), 5000);
        return;
    }
    pandoc::ImportRequest request;
    request.reader = reader;
    request.inputPath = input.absoluteFilePath();
    request.outputName = QStringLiteral("out.md");

    runPandoc(program, pandoc::importArguments(request), {}, scratch->path(), tr("Import with pandoc"),
              [this, scratch, input, folderRelative, reader, vaultRoot = m_vault.root()] {
                  if (m_vault.root() != vaultRoot) { // another vault opened while pandoc ran: the note was meant for the first
                      statusBar()->showMessage(tr("%1 was not imported: the vault changed meanwhile").arg(input.fileName()), 6000);
                      return;
                  }
                  QFile result(scratch->filePath(QStringLiteral("out.md")));
                  if (QFileInfo(result).isSymLink()) { // a link is not pandoc's output
                      showNotice(this, QStringLiteral("pandocNotAFileDialog"), tr("Import with pandoc"),
                                 tr("pandoc did not write a file, so nothing was imported."));
                      return;
                  }
                  if (!result.open(QIODevice::ReadOnly)) {
                      QMessageBox::warning(this, tr("Import with pandoc"), tr("pandoc wrote no Markdown."));
                      return;
                  }
                  QString text = QString::fromUtf8(result.readAll());
                  // Obsidian's own invalid characters, and no leading dot, which would hide the note.
                  QString base = input.completeBaseName();
                  base.replace(QRegularExpression(QStringLiteral("[#^\\[\\]|:\\\\/]")), QStringLiteral("-"));
                  while (base.startsWith(QLatin1Char('.')))
                      base.remove(0, 1);
                  if (base.isEmpty())
                      base = tr("Imported");
                  // The attachment folder depends on the note's folder only, so the pictures can go first and the
                  // note be made once, whole: a failure never leaves an empty note behind.
                  const QString folderAbsolute = folderRelative.isEmpty() ? m_vault.root() : m_vault.absolute(folderRelative);
                  const QString attachments = m_vault.attachmentFolderFor(
                      m_settings.text(QStringLiteral("files.attachmentFolder")), QDir(folderAbsolute).filePath(base + QStringLiteral(".md")));
                  QString mediaError;
                  QHash<QString, QString> names = pandoc::moveMedia(scratch->path(), attachments, &mediaError);
                  // Pictures the sandboxed reader left as links beside the document (HTML, LaTeX).
                  // The readers that pull their pictures into the file's own package have extracted them already.
                  if (!QStringList{QStringLiteral("docx"), QStringLiteral("odt"), QStringLiteral("epub"), QStringLiteral("rtf")}.contains(reader)) {
                      QString siblingError;
                      names.insert(pandoc::moveSiblingImages(text, input.absolutePath(), attachments, &siblingError));
                      if (mediaError.isEmpty())
                          mediaError = siblingError;
                  }
                  // Never over a note that is there: the vault numbers a name taken ("Name 1").
                  QString error;
                  const QString path = m_vault.createNote(folderRelative, base, &error, QStringLiteral(".md"),
                                                          pandoc::rewriteImported(text, names));
                  if (path.isEmpty()) {
                      for (const QString &name : std::as_const(names)) { // no note will show them: take them back out
                          if (!name.isEmpty())
                              QFile::remove(QDir(attachments).filePath(name));
                      }
                      QDir().rmdir(attachments); // only when that left it empty
                      QMessageBox::warning(this, tr("Import with pandoc"), tr("Could not create the note: %1").arg(error));
                      return;
                  }
                  m_indexer->reindex(path);
                  openNote(path);
                  statusBar()->showMessage(tr("Imported %1").arg(QFileInfo(path).fileName()), 5000);
                  if (!mediaError.isEmpty()) {
                      showNotice(this, QStringLiteral("pandocMediaDialog"), tr("Import with pandoc"),
                                 tr("The note was made, but a picture could not be copied into the vault: %1").arg(mediaError));
                  }
              },
              {input.canonicalFilePath()}); // the one file it may read
}

} // namespace noteAppC
