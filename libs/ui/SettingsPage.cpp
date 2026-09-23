#include "SettingsPage.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "HtmlRenderer.h"
#include "Hotkeys.h"
#include "MediaPlayback.h"
#include "PdfPage.h"
#include "Scheme.h"
#include "Settings.h"
#include "Theme.h"
#include "ToggleSwitch.h"

namespace noteAppC {

namespace {
QWidget *scrollWrap(QWidget *content)
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}
} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("settingsPage"));

    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("settingsNav"));
    m_nav->setFixedWidth(200);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->addItems({tr("Appearance"), tr("Editor"), tr("Files and links"), tr("Core plugins"),
                     tr("Hotkeys"), tr("About")});

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("settingsPages"));

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_nav);
    layout->addWidget(m_pages, 1);

    setFocusPolicy(Qt::StrongFocus);
}

void SettingsPage::setContext(Settings *settings, const QString &vaultRoot)
{
    m_settings = settings;
    m_vaultRoot = vaultRoot;
    m_needsRebuild = true;
    // Building the four pages costs 12 ms, it happened twice before a window
    // was even on screen, and most sessions never open settings at all. It
    // waits for something to show the page.
    if (isVisible())
        rebuild();
}

void SettingsPage::rebuild()
{
    m_needsRebuild = false;
    m_rows.clear();

    const int row = qMax(0, m_nav->currentRow());
    while (m_pages->count() > 0) {
        QWidget *page = m_pages->widget(0);
        m_pages->removeWidget(page);
        page->deleteLater();
    }
    if (!m_settings)
        return;

    m_pages->addWidget(buildAppearancePage());
    m_pages->addWidget(buildEditorPage());
    m_pages->addWidget(buildFilesPage());
    m_pages->addWidget(buildCorePluginsPage());
    m_pages->addWidget(buildHotkeysPage());
    m_pages->addWidget(buildAboutPage());
    m_nav->setCurrentRow(row < m_pages->count() ? row : 0);
    refreshRowDecorations();
    if (m_theme)
        applyTheme(*m_theme); // the rows just made were not there for the last one
}

void SettingsPage::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit closeRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SettingsPage::hideEvent(QHideEvent *event)
{
    // Rebuilt on the next open, so a theme added where nothing was watching -
    // a themes directory created after launch - is still listed then. It
    // costs nothing until someone opens the page again.
    m_needsRebuild = true;
    QWidget::hideEvent(event);
}

void SettingsPage::showEvent(QShowEvent *event)
{
    if (m_needsRebuild)
        rebuild();
    QWidget::showEvent(event);
    setFocus();
}

QWidget *SettingsPage::addRow(QVBoxLayout *into, const QStringList &keys, const QString &label,
                              const QString &description, QWidget *control,
                              std::function<void()> resetControl)
{
    auto *row = new QWidget(this);
    row->setObjectName(QStringLiteral("settingsRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 14, 0, 14);
    rowLayout->setSpacing(16);

    auto *text = new QWidget(row);
    auto *textLayout = new QVBoxLayout(text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    auto *labelWidget = new QLabel(label, text);
    labelWidget->setObjectName(QStringLiteral("settingsRowLabel"));
    textLayout->addWidget(labelWidget);
    if (!description.isEmpty()) {
        auto *descWidget = new QLabel(description, text);
        descWidget->setObjectName(QStringLiteral("settingsRowDescription"));
        descWidget->setWordWrap(true);
        textLayout->addWidget(descWidget);
    }

    auto *controlsRow = new QWidget(row);
    auto *controlsLayout = new QHBoxLayout(controlsRow);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    auto *vaultTag = new QLabel(tr("set by this vault"), controlsRow);
    vaultTag->setObjectName(QStringLiteral("settingsVaultTag"));
    vaultTag->setVisible(false);

    auto *reset = new QToolButton(controlsRow);
    reset->setObjectName(QStringLiteral("settingsResetButton"));
    reset->setText(tr("Reset"));
    reset->setAutoRaise(true);
    reset->setCursor(Qt::PointingHandCursor);
    reset->setVisible(false);
    connect(reset, &QToolButton::clicked, this, [this, keys, control, resetControl] {
        for (const QString &key : keys)
            m_settings->reset(key);
        if (resetControl) {
            const QSignalBlocker blocker(control);
            resetControl();
        }
        changed();
    });

    controlsLayout->addWidget(vaultTag);
    controlsLayout->addWidget(reset);
    controlsLayout->addWidget(control);

    rowLayout->addWidget(text, 1);
    rowLayout->addWidget(controlsRow, 0);

    auto *container = new QWidget(this);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);
    containerLayout->addWidget(row);
    auto *hairline = new QFrame(container);
    hairline->setObjectName(QStringLiteral("settingsHairline"));
    hairline->setFixedHeight(1);
    containerLayout->addWidget(hairline);

    into->addWidget(container);

    Row entry;
    entry.keys = keys;
    entry.reset = reset;
    entry.vaultTag = vaultTag;
    m_rows.append(entry);

    return container;
}

void SettingsPage::addToggleRow(QVBoxLayout *into, const QString &key, const QString &label,
                                const QString &description)
{
    auto *toggle = new ToggleSwitch(this);
    toggle->setObjectName(QStringLiteral("settingsToggle_") + key);
    toggle->setChecked(m_settings->boolean(key));
    connect(toggle, &QAbstractButton::toggled, this, [this, key](bool on) {
        m_settings->setValue(key, on);
        changed();
    });
    addRow(into, {key}, label, description, toggle,
          [this, key, toggle] { toggle->setChecked(m_settings->boolean(key)); });
}

void SettingsPage::addComboRow(QVBoxLayout *into, const QString &key, const QString &label,
                               const QString &description,
                               const QList<QPair<QString, QVariant>> &options)
{
    auto *combo = new QComboBox(this);
    combo->setObjectName(QStringLiteral("settingsCombo_") + key);
    for (const auto &option : options)
        combo->addItem(option.first, option.second);

    const auto sync = [this, key, combo] {
        const int index = combo->findData(m_settings->value(key));
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };
    sync();
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
           [this, key, combo](int index) {
               m_settings->setValue(key, combo->itemData(index));
               changed();
           });
    addRow(into, {key}, label, description, combo, sync);
}

void SettingsPage::addSpinRow(QVBoxLayout *into, const QString &key, const QString &label,
                              const QString &description, int minimum, int maximum,
                              const QString &suffix)
{
    auto *spin = new QSpinBox(this);
    spin->setObjectName(QStringLiteral("settingsSpin_") + key);
    spin->setRange(minimum, maximum);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);
    spin->setValue(m_settings->integer(key));
    connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, key](int value) {
        m_settings->setValue(key, value);
        changed();
    });
    addRow(into, {key}, label, description, spin,
          [this, key, spin] { spin->setValue(m_settings->integer(key)); });
}

void SettingsPage::addLineEditRow(QVBoxLayout *into, const QString &key, const QString &label,
                                  const QString &description, const QString &placeholder)
{
    auto *edit = new QLineEdit(this);
    edit->setObjectName(QStringLiteral("settingsLineEdit_") + key);
    edit->setPlaceholderText(placeholder);
    edit->setText(m_settings->text(key));
    edit->setMinimumWidth(160);
    connect(edit, &QLineEdit::textEdited, this, [this, key](const QString &text) {
        m_settings->setValue(key, text);
        changed();
    });
    addRow(into, {key}, label, description, edit,
          [this, key, edit] { edit->setText(m_settings->text(key)); });
}

void SettingsPage::addAccentRow(QVBoxLayout *into)
{
    static const QString hueKey = QStringLiteral("appearance.accentHue");
    static const QString satKey = QStringLiteral("appearance.accentSaturation");
    static const QString lightKey = QStringLiteral("appearance.accentLightness");
    const QStringList keys{hueKey, satKey, lightKey};

    auto *swatch = new QToolButton(this);
    swatch->setObjectName(QStringLiteral("settingsAccentSwatch"));
    swatch->setFixedSize(28, 28);
    swatch->setCursor(Qt::PointingHandCursor);

    const auto currentColor = [this] {
        return QColor::fromHsl(m_settings->integer(hueKey),
                               qRound(m_settings->integer(satKey) * 2.55),
                               qRound(m_settings->integer(lightKey) * 2.55));
    };
    const auto sync = [swatch, currentColor] {
        swatch->setStyleSheet(
            QStringLiteral(
                "QToolButton{background:%1;border:1px solid palette(mid);border-radius:6px;}")
                .arg(currentColor().name()));
    };
    sync();
    connect(swatch, &QToolButton::clicked, this, [this, sync, currentColor] {
        const QColor picked = QColorDialog::getColor(currentColor(), this, tr("Accent colour"));
        if (!picked.isValid())
            return;
        m_settings->setValue(hueKey, qMax(0, picked.hslHue()));
        m_settings->setValue(satKey, qRound(picked.hslSaturationF() * 100));
        m_settings->setValue(lightKey, qRound(picked.lightnessF() * 100));
        sync();
        changed();
    });
    addRow(into, keys, tr("Accent colour"),
          tr("Used for links, the active tab and every other interactive highlight."), swatch,
          sync);
}

void SettingsPage::addSchemeRow(QVBoxLayout *into)
{
    static const QString key = QStringLiteral("appearance.scheme");

    auto *combo = new QComboBox(this);
    combo->setObjectName(QStringLiteral("settingsSchemeCombo"));
    combo->addItem(tr("Built-in"), QString());
    for (const SchemeInfo &scheme : Scheme::available(m_vaultRoot)) {
        combo->addItem(scheme.obsidianCSS ? tr("%1 (Obsidian theme)").arg(scheme.name)
                                          : scheme.name,
                      scheme.path);
    }

    const auto sync = [this, combo] {
        int index = combo->findData(m_settings->text(key));
        // A scheme name with no matching path on disk any more - or an
        // Obsidian cssTheme name carried over as-is by fromObsidianConfig,
        // which is not a path at all - falls back to Built-in rather than
        // showing nothing selected.
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };
    sync();
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
           [this, combo](int index) {
               m_settings->setValue(key, combo->itemData(index).toString());
               changed();
           });
    addRow(into, {key}, tr("Colour scheme"),
          tr("A noteAppC scheme, or the colours of an Obsidian theme already in this vault."),
          combo, sync);
}

void SettingsPage::addFolderRow(QVBoxLayout *into, const QString &key, const QString &label,
                                const QString &description)
{
    auto *wrap = new QWidget(this);
    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *edit = new QLineEdit(wrap);
    edit->setObjectName(QStringLiteral("settingsFolderEdit"));
    edit->setPlaceholderText(tr("Vault root"));
    edit->setText(m_settings->text(key));
    edit->setMinimumWidth(160);

    auto *browse = new QToolButton(wrap);
    browse->setObjectName(QStringLiteral("settingsFolderBrowse"));
    browse->setText(QStringLiteral("..."));
    browse->setCursor(Qt::PointingHandCursor);

    layout->addWidget(edit, 1);
    layout->addWidget(browse);

    connect(edit, &QLineEdit::textEdited, this, [this, key](const QString &text) {
        m_settings->setValue(key, text);
        changed();
    });
    connect(browse, &QToolButton::clicked, this, [this, key, edit] {
        const QString start = m_vaultRoot.isEmpty() ? QDir::homePath() : m_vaultRoot;
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Attachment folder"), start);
        if (chosen.isEmpty())
            return;
        const QString relative =
            m_vaultRoot.isEmpty() ? chosen : QDir(m_vaultRoot).relativeFilePath(chosen);
        edit->setText(relative);
        m_settings->setValue(key, relative);
        changed();
    });

    addRow(into, {key}, label, description, wrap,
          [this, key, edit] { edit->setText(m_settings->text(key)); });
}

QWidget *SettingsPage::buildAppearancePage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsAppearancePage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(0);

    addComboRow(layout, QStringLiteral("appearance.theme"), tr("Appearance"),
               tr("Dark, true black for OLED, light, or match the desktop."),
               {{tr("Dark"), QStringLiteral("dark")},
                {tr("Black (OLED)"), QStringLiteral("oled")},
                {tr("Light"), QStringLiteral("light")},
                {tr("Same as system"), QStringLiteral("system")}});
    addSchemeRow(layout);
    addAccentRow(layout);
    addSpinRow(layout, QStringLiteral("appearance.baseFontSize"), tr("Font size"),
              tr("Size of the note text."), 8, 32, tr(" px"));

    // A family fontconfig does not have is ignored rather than substituted,
    // so a typo leaves the built-in list in place instead of changing the
    // look of the whole application - see Theme::setFontOverrides.
    addLineEditRow(layout, QStringLiteral("appearance.interfaceFont"), tr("Interface font"),
                  tr("Family used for menus and panels."), tr("System default"));
    addLineEditRow(layout, QStringLiteral("appearance.textFont"), tr("Note text font"),
                  tr("Family used for the note body."), tr("System default"));
    addLineEditRow(layout, QStringLiteral("appearance.monospaceFont"), tr("Monospace font"),
                  tr("Family used for code."), tr("System default"));

    addToggleRow(layout, QStringLiteral("appearance.showInlineTitle"), tr("Show inline title"),
                tr("Show the note's file name as a heading above its text."));
    // appearance.showRibbon has no control here: the ribbon this key used to
    // gate is gone, replaced by the header bar, and nothing in the new shell
    // reads it. Left alone since Settings' key list is not this session's.

    layout->addStretch(1);
    return scrollWrap(page);
}

QWidget *SettingsPage::buildEditorPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsEditorPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(0);

    addComboRow(layout, QStringLiteral("editor.defaultMode"), tr("Default view"),
               tr("How a note opens: live preview, raw source, or reading only."),
               {{tr("Live preview"), QStringLiteral("livePreview")},
                {tr("Source"), QStringLiteral("source")},
                {tr("Reading"), QStringLiteral("reading")}});
    addToggleRow(layout, QStringLiteral("editor.readableLineLength"), tr("Readable line length"),
                tr("Keep lines from stretching edge to edge on a wide window."));
    addSpinRow(layout, QStringLiteral("editor.contentWidth"), tr("Line width"),
              tr("Maximum width of the text column."), 400, 1200, tr(" px"));
    addToggleRow(layout, QStringLiteral("editor.lineNumbers"), tr("Line numbers"),
                tr("Show line numbers in source mode."));
    addToggleRow(layout, QStringLiteral("editor.autoPairs"), tr("Auto-pair brackets and markdown"),
                tr("Close brackets and quotes as you type, and wrap a selection instead of "
                   "replacing it."));

    addToggleRow(layout, QStringLiteral("editor.strictLineBreaks"), tr("Strict line breaks"),
                tr("Treat every newline as a line break, the way CommonMark does."));
#ifdef NOTEAPPC_HAVE_SPELLCHECK
    // Only in a build with the spellcheck module: without it there is nothing
    // behind the switch, and a switch that does nothing is worse than none.
    addToggleRow(layout, QStringLiteral("editor.spellcheck"), tr("Spellcheck"),
                tr("Underline words the dictionary does not recognise. Uses the hunspell "
                   "dictionaries installed for your system's languages."));
#endif
    addToggleRow(layout, QStringLiteral("editor.showIndentGuides"), tr("Indent guides"),
                tr("Show a vertical line at each level of list indentation."));
    addSpinRow(layout, QStringLiteral("editor.tabWidth"), tr("Tab width"),
              tr("Spaces a Tab keypress inserts."), 1, 8);
    addToggleRow(layout, QStringLiteral("editor.gpuAcceleration"), tr("GPU acceleration"),
                tr("Draw the note through OpenGL instead of the CPU. Measured slower on "
                   "average for text, with a steadier worst case - try it on your hardware."));

    layout->addStretch(1);
    return scrollWrap(page);
}

QWidget *SettingsPage::buildFilesPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsFilesPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(0);

    addFolderRow(layout, QStringLiteral("files.attachmentFolder"), tr("Attachment folder"),
                tr("Where a pasted or dropped file lands. Empty means the vault root."));
    addToggleRow(layout, QStringLiteral("files.autosave"), tr("Save automatically"),
                tr("Save a note a moment after typing stops, and before it is closed or left, "
                   "as Obsidian does. Off asks before closing a note with unsaved changes."));
    addToggleRow(layout, QStringLiteral("files.confirmDelete"), tr("Confirm before deleting"),
                tr("Ask before moving a note or folder to the vault's trash."));
    addToggleRow(layout, QStringLiteral("links.useWikilinks"), tr("Use [[wikilinks]]"),
                tr("Write new links as [[Note]] instead of [Note](Note.md)."));
    addToggleRow(layout, QStringLiteral("links.autoUpdate"), tr("Update links on rename"),
                tr("Rewrite every link that points at a note when it is renamed or moved."));
    addSpinRow(layout, QStringLiteral("fileRecovery.interval"), tr("Snapshot interval"),
              tr("File recovery keeps a copy of a note when it is saved, at most this many "
                 "minutes apart."),
              1, 120);
    addSpinRow(layout, QStringLiteral("fileRecovery.keepDays"), tr("Snapshot history"),
              tr("Days a snapshot is kept before it is deleted."), 1, 365);
    auto *view = new QPushButton(tr("View"));
    view->setObjectName(QStringLiteral("settingsRecoveryButton"));
    connect(view, &QPushButton::clicked, this, &SettingsPage::recoveryRequested);
    addRow(layout, {}, tr("Snapshots"),
           tr("Earlier versions of your notes, kept outside the vault."), view, {});

    layout->addStretch(1);
    return scrollWrap(page);
}

void SettingsPage::setCommands(const QList<HotkeyCommand> &commands)
{
    m_commands = commands;
    std::sort(m_commands.begin(), m_commands.end(),
              [](const HotkeyCommand &l, const HotkeyCommand &r) {
                  return l.name.localeAwareCompare(r.name) < 0;
              });
    m_needsRebuild = true;
    if (isVisible())
        rebuild();
}

QList<QKeySequence> SettingsPage::effectiveKeys(const HotkeyCommand &command) const
{
    const Hotkeys::Bindings bindings = Hotkeys::load(m_vaultRoot);
    const auto found = bindings.constFind(command.id);
    return found != bindings.constEnd() ? *found : command.builtIn;
}

QWidget *SettingsPage::buildCorePluginsPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsCorePluginsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(0);

    // Only what this build has: a switch with nothing behind it is worse
    // than none.
    QSet<QByteArray> missing = {
#ifndef NOTEAPPC_HAVE_GRAPH
        "graph",
#endif
#ifndef NOTEAPPC_HAVE_CANVAS
        "canvas",
#endif
#ifndef NOTEAPPC_HAVE_BASES
        "bases",
#endif
#ifndef NOTEAPPC_HAVE_HISTORY
        "note-history",
#endif
    };
    if (!PdfPage::isAvailable())
        missing << "pdf-embeds";
    if (!MediaPlayback::isAvailable())
        missing << "media-embeds";
    if (!HtmlRenderer::isAvailable())
        missing << "html-blocks";
    for (const Settings::Feature &feature : Settings::features()) {
        if (!missing.contains(feature.id))
            addToggleRow(layout, Settings::featureKey(feature.id), QCoreApplication::translate("Settings", feature.name),
                        QCoreApplication::translate("Settings", feature.description));
    }

    // A vault's own .obsidian/daily-notes.json and templates.json win over
    // these, so an Obsidian vault behaves here as it did there.
    addFolderRow(layout, QStringLiteral("dailyNotes.folder"), tr("Daily notes folder"),
                tr("Where today's note is created. Empty means the vault root."));
    addLineEditRow(layout, QStringLiteral("dailyNotes.format"), tr("Daily note date format"),
                   tr("Its file name, in Obsidian's moment.js tokens."), QStringLiteral("YYYY-MM-DD"));
    addLineEditRow(layout, QStringLiteral("dailyNotes.template"), tr("Daily note template"),
                   tr("A note to start each day's note from, as a vault path without \".md\"."));
    addFolderRow(layout, QStringLiteral("templates.folder"), tr("Templates folder"),
                tr("Where Insert template looks."));
    addLineEditRow(layout, QStringLiteral("templates.dateFormat"), tr("Template date format"),
                   tr("What {{date}} becomes."), QStringLiteral("YYYY-MM-DD"));
    addLineEditRow(layout, QStringLiteral("templates.timeFormat"), tr("Template time format"),
                   tr("What {{time}} becomes."), QStringLiteral("HH:mm"));
    layout->addStretch(1);
    return scrollWrap(page);
}

QWidget *SettingsPage::buildHotkeysPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsHotkeysPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(0);

    auto *filter = new QLineEdit(page);
    filter->setObjectName(QStringLiteral("settingsHotkeysFilter"));
    filter->setPlaceholderText(tr("Filter commands..."));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    layout->addSpacing(8);

    // Who holds which key, so a clash shows on both commands' rows.
    const Hotkeys::Bindings own = Hotkeys::loadOwn();
    QHash<int, QStringList> holders;
    QHash<QString, QList<QKeySequence>> current;
    for (const HotkeyCommand &command : std::as_const(m_commands)) {
        current.insert(command.id, effectiveKeys(command));
        for (const QKeySequence &keys : current.value(command.id)) {
            if (!keys.isEmpty())
                holders[keys[0].toCombined()] << command.name;
        }
    }

    QList<QWidget *> rows;
    for (const HotkeyCommand &command : std::as_const(m_commands)) {
        auto *row = new QWidget(page);
        row->setObjectName(QStringLiteral("settingsRow"));
        row->setProperty("commandText", (command.name + QLatin1Char(' ') + command.id).toLower());
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 10, 0, 10);
        rowLayout->setSpacing(16);

        QString description = command.id;
        for (const QKeySequence &keys : current.value(command.id)) {
            if (keys.isEmpty())
                continue;
            QStringList others = holders.value(keys[0].toCombined());
            others.removeOne(command.name);
            if (!others.isEmpty())
                description += tr(" - %1 is also bound to %2")
                                   .arg(keys.toString(QKeySequence::NativeText), others.join(tr(", ")));
        }
        auto *text = new QWidget(row);
        auto *textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);
        auto *label = new QLabel(command.name, text);
        label->setObjectName(QStringLiteral("settingsRowLabel"));
        auto *id = new QLabel(description, text);
        id->setObjectName(QStringLiteral("settingsRowDescription"));
        id->setWordWrap(true);
        textLayout->addWidget(label);
        textLayout->addWidget(id);

        auto *edit = new QKeySequenceEdit(row);
        edit->setObjectName(QStringLiteral("settingsHotkeyEdit"));
        edit->setMaximumSequenceLength(1); // one chord, as Obsidian takes
        edit->setClearButtonEnabled(true);
        const QList<QKeySequence> keys = current.value(command.id);
        edit->setKeySequence(keys.isEmpty() ? QKeySequence() : keys.first());
        edit->setMinimumWidth(110);

        auto *reset = new QToolButton(row);
        reset->setObjectName(QStringLiteral("settingsResetButton"));
        reset->setText(tr("Reset"));
        reset->setAutoRaise(true);
        reset->setCursor(Qt::PointingHandCursor);
        reset->setVisible(own.contains(command.id));

        const QString commandID = command.id;
        connect(edit, &QKeySequenceEdit::editingFinished, this, [this, edit, commandID] {
            Hotkeys::Bindings bindings = Hotkeys::loadOwn();
            const QKeySequence keys = edit->keySequence();
            bindings.insert(commandID, keys.isEmpty() ? QList<QKeySequence>() : QList{keys});
            Hotkeys::save(bindings);
            emit hotkeysChanged();
            m_needsRebuild = true; // clash notes and Reset buttons follow on reopen
        });
        connect(reset, &QToolButton::clicked, this, [this, commandID] {
            Hotkeys::Bindings bindings = Hotkeys::loadOwn();
            bindings.remove(commandID);
            Hotkeys::save(bindings);
            emit hotkeysChanged();
            rebuild();
        });

        rowLayout->addWidget(text, 1);
        rowLayout->addWidget(reset, 0);
        rowLayout->addWidget(edit, 0);
        layout->addWidget(row);
        auto *hairline = new QFrame(page);
        hairline->setObjectName(QStringLiteral("settingsHairline"));
        hairline->setFixedHeight(1);
        layout->addWidget(hairline);
        rows << row << hairline;
    }
    layout->addStretch(1);

    connect(filter, &QLineEdit::textChanged, page, [rows](const QString &text) {
        const QString needle = text.trimmed().toLower();
        for (int i = 0; i + 1 < rows.size(); i += 2) {
            const bool shown =
                needle.isEmpty() || rows.at(i)->property("commandText").toString().contains(needle);
            rows.at(i)->setVisible(shown);
            rows.at(i + 1)->setVisible(shown);
        }
    });
    return scrollWrap(page);
}

QWidget *SettingsPage::buildAboutPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("settingsAboutPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 20, 28, 20);
    layout->setSpacing(6);

    auto *name = new QLabel(QStringLiteral("noteAppC"), page);
    name->setObjectName(QStringLiteral("settingsAboutName"));
    QFont nameFont = name->font();
    nameFont.setPointSize(nameFont.pointSize() + 8);
    nameFont.setBold(true);
    name->setFont(nameFont);

    auto *version = new QLabel(tr("Version %1").arg(QStringLiteral(NOTEAPPC_VERSION)), page);
    version->setObjectName(QStringLiteral("settingsAboutVersion"));

    auto *description = new QLabel(
        tr("A Linux-native alternative to Obsidian: C++ and Qt Widgets, no JavaScript, no "
           "browser engine."),
        page);
    description->setObjectName(QStringLiteral("settingsAboutDescription"));
    description->setWordWrap(true);

    auto *configPath = new QLabel(tr("Settings file: %1").arg(Settings::configFile()), page);
    configPath->setObjectName(QStringLiteral("settingsAboutConfigPath"));
    configPath->setWordWrap(true);

    layout->addWidget(name);
    layout->addWidget(version);
    layout->addSpacing(12);
    layout->addWidget(description);
    layout->addSpacing(12);
    layout->addWidget(configPath);
    layout->addStretch(1);
    return scrollWrap(page);
}

void SettingsPage::refreshRowDecorations()
{
    if (!m_settings)
        return;
    for (const Row &row : std::as_const(m_rows)) {
        const bool custom =
            std::any_of(row.keys.begin(), row.keys.end(), [this](const QString &key) {
                return m_settings->value(key) != Settings::defaultValue(key);
            });
        row.reset->setVisible(custom);
        const bool fromVault =
            std::any_of(row.keys.begin(), row.keys.end(),
                       [this](const QString &key) { return m_settings->isFromVault(key); });
        row.vaultTag->setVisible(fromVault);
    }
}

void SettingsPage::changed()
{
    refreshRowDecorations();
    emit settingChanged();
}

void SettingsPage::applyTheme(const Theme &theme)
{
    m_theme = theme;
    for (ToggleSwitch *toggle : findChildren<ToggleSwitch *>())
        toggle->setTheme(theme);

    const QString sheet =
        QStringLiteral(R"(
QWidget#settingsPage { background: %1; }
QListWidget#settingsNav {
    background: %2;
    border: none;
    border-right: 1px solid %3;
    padding: 12px 6px;
    font-size: 13px;
    color: %4;
    outline: none;
}
QListWidget#settingsNav::item { padding: 8px 10px; border-radius: 6px; margin: 1px 0; }
QListWidget#settingsNav::item:hover { background: %5; }
QListWidget#settingsNav::item:selected { background: %6; color: %7; }
QLabel#settingsRowLabel { color: %7; font-size: 14px; font-weight: 600; }
QLabel#settingsRowDescription { color: %4; font-size: 12px; }
QFrame#settingsHairline { background: %3; border: none; }
QLabel#settingsVaultTag {
    color: %4;
    font-size: 11px;
    background: %5;
    border-radius: 4px;
    padding: 2px 6px;
}
QToolButton#settingsResetButton { color: %8; font-size: 12px; border: none; }
QToolButton#settingsResetButton:hover { text-decoration: underline; }
QLabel#settingsAboutDescription, QLabel#settingsAboutConfigPath { color: %4; font-size: 12px; }
)")
            .arg(theme.value(QStringLiteral("--background-primary")),
                theme.value(QStringLiteral("--background-secondary")),
                theme.value(QStringLiteral("--background-modifier-border")),
                theme.value(QStringLiteral("--text-muted")))
            .arg(theme.value(QStringLiteral("--background-modifier-hover")),
                theme.value(QStringLiteral("--background-modifier-active")),
                theme.value(QStringLiteral("--text-normal")),
                theme.value(QStringLiteral("--interactive-accent")));
    setStyleSheet(sheet);
}

} // namespace noteAppC
