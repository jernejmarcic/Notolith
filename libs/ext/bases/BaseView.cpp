#include "BaseView.h"

#include "BaseCellDelegate.h"
#include "BaseQuery.h"
#include "BaseTableModel.h"
#include "Index.h"
#include "Vault.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace noteAppC::bases {

BaseView::BaseView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *toolbar = new QHBoxLayout;
    m_viewSwitcher = new QComboBox(this);
    connect(m_viewSwitcher, &QComboBox::currentIndexChanged, this, &BaseView::onViewSwitched);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: palette(mid);"));
    toolbar->addWidget(m_viewSwitcher);
    toolbar->addWidget(m_status, 1);
    layout->addLayout(toolbar);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    m_table = new QTableView(this);
    // Sorted by a header click, and the indicator set from the view's own
    // `sort:` without sorting again. With Qt's sorting switched on, the
    // header showed its default - the first column, descending - over rows
    // in no such order.
    m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    connect(m_table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
            &BaseView::onHeaderSortChanged);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionsMovable(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->setItemDelegate(new BaseCellDelegate(m_table));
    connect(m_table, &QTableView::doubleClicked, this, &BaseView::onTableActivated);
    connect(m_table, &QTableView::clicked, this, &BaseView::onTableClicked);
    m_stack->addWidget(m_table);

    m_cardsArea = new QScrollArea(this);
    m_cardsArea->setWidgetResizable(true);
    m_cardsContent = new QWidget;
    m_cardsLayout = new QGridLayout(m_cardsContent);
    m_cardsArea->setWidget(m_cardsContent);
    m_stack->addWidget(m_cardsArea);

    connect(&m_opening, &QFutureWatcher<Opened>::finished, this, &BaseView::showOpenedView);
}

BaseView::~BaseView()
{
    // Children are destroyed after members, and a shown table asks its model
    // for columns while it hides: the model reads the query, gone by then.
    m_table->setModel(nullptr);
}

void BaseView::setIndex(Index *index) { m_index = index; }
void BaseView::setVault(const Vault *vault) { m_vault = vault; }
void BaseView::setLinkResolver(std::function<QString(const QString &)> resolver) { m_linkResolver = std::move(resolver); }

void BaseView::clear()
{
    m_file.reset();
    m_query = nullptr;
    m_table->setModel(nullptr);
    delete m_tableModel;
    m_tableModel = nullptr;
    m_shown = Opened{};
    rebuildCards();
    m_viewSwitcher->blockSignals(true);
    m_viewSwitcher->clear();
    m_viewSwitcher->blockSignals(false);
    m_status->clear();
    m_lastError.clear();
    m_path.clear();
    m_yamlText.clear();
    m_imageCache.clear();
}

bool BaseView::open(const QString &absolutePath, const QString &yamlText)
{
    clear();
    QString error;
    auto parsed = BaseFile::parse(yamlText, &error);
    if (!parsed) {
        m_lastError = error;
        return false;
    }
    m_path = absolutePath;
    m_yamlText = yamlText;
    m_file = std::make_shared<const BaseFile>(std::move(*parsed));
    m_currentView = 0;
    rebuildViewSwitcher();
    openCurrentView();
    return true;
}

void BaseView::rebuildViewSwitcher()
{
    m_viewSwitcher->blockSignals(true);
    m_viewSwitcher->clear();
    if (m_file) {
        int i = 0;
        for (const ViewConfig &v : m_file->views()) {
            ++i;
            m_viewSwitcher->addItem(v.name.isEmpty() ? tr("View %1").arg(i) : v.name);
        }
    }
    m_viewSwitcher->setCurrentIndex(m_currentView);
    m_viewSwitcher->blockSignals(false);
}

void BaseView::onViewSwitched(int index)
{
    if (index < 0 || !m_file || (index == m_currentView && m_query))
        return;
    m_currentView = index;
    openCurrentView();
}

void BaseView::openCurrentView()
{
    if (!m_file || !m_index) {
        m_status->setText(m_index ? QString() : tr("no index"));
        return;
    }

    // The task reads the cache through its own connections and holds only a
    // copy of its path, never the Index: a task left running when the view or
    // the Index goes, or the Index reopens on another vault, touches nothing
    // freed. A newer request replaces this one, and the watcher drops its result.
    m_status->setText(tr("Loading..."));
    m_opening.setFuture(QtConcurrent::run(
        [databasePath = m_index->databasePath(), file = m_file, view = m_currentView,
         vault = std::make_shared<const Vault>(m_vault ? *m_vault : Vault())] {
            QList<Row> rows;
            for (const Index::BaseNote &note : Index::notesForBases(databasePath))
                rows << rowFromNote(note);
            Opened opened;
            opened.file = file;
            opened.vault = vault;
            opened.query = std::make_shared<BaseQuery>(*file, std::move(rows), vault.get());
            if (!opened.query->openView(view, &opened.error))
                opened.query.reset();
            return opened;
        }));
}

void BaseView::showOpenedView()
{
    const Opened opened = m_opening.result();
    if (!opened.query) {
        m_status->setText(opened.error);
        emit viewOpened();
        return;
    }
    // The model goes before the query it points into.
    m_table->setModel(nullptr);
    delete m_tableModel;
    m_tableModel = nullptr;
    m_shown = opened;
    m_query = m_shown.query.get();

    m_tableModel = new BaseTableModel(m_query, m_index, m_vault, this);
    m_tableModel->setBeforeWrite(m_beforeWrite);
    connect(m_tableModel, &BaseTableModel::noteEdited, this, &BaseView::onCellEdited);
    m_table->setModel(m_tableModel);
    {
        // The indicator shows the view's own first sort key, and setting it
        // must not sort again.
        const QSignalBlocker blocker(m_table->horizontalHeader());
        const ViewConfig *shown = view();
        const bool sorted = shown && !shown->sort.isEmpty();
        m_table->horizontalHeader()->setSortIndicator(
            sorted ? m_tableModel->columnOf(shown->sort.constFirst().property) : -1,
            sorted && shown->sort.constFirst().direction == SortDirection::Descending ? Qt::DescendingOrder
                                                                                      : Qt::AscendingOrder);
    }

    const bool cards = view() && view()->type.compare(QLatin1String("cards"), Qt::CaseInsensitive) == 0;
    m_stack->setCurrentWidget(cards ? static_cast<QWidget *>(m_cardsArea) : m_table);
    if (cards)
        rebuildCards();

    const QStringList &warn = m_query->warnings();
    m_status->setText(warn.isEmpty()
        ? tr("%1 notes").arg(m_query->resultRows().size())
        : tr("%1 notes - %2 warning(s)").arg(m_query->resultRows().size()).arg(warn.size()));
    emit viewOpened();
}

const ViewConfig *BaseView::view() const
{
    return m_file && m_currentView >= 0 && m_currentView < m_file->views().size()
        ? &m_file->views().at(m_currentView) : nullptr;
}

void BaseView::onTableActivated(const QModelIndex &index)
{
    if (!index.isValid() || !m_tableModel)
        return;
    const QString path = m_tableModel->notePathAt(index.row());
    if (!path.isEmpty())
        emit noteActivated(path);
}

void BaseView::onTableClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_tableModel)
        return;
    if (m_tableModel->isNameColumn(index.column())) {
        onTableActivated(index);
        return;
    }
    // A Link cell (or a list cell holding one): open it, same as file.name -
    // the coarse simplification is a list of several links opens the
    // *first*, not whichever chip the click landed on; per-chip hit-testing
    // is the upgrade if that ever proves too coarse in practice.
    const Value value = m_tableModel->valueAt(index.row(), index.column());
    if (value.type == Value::Type::Link) {
        openLinkTarget(value.text);
    } else if (value.type == Value::Type::List) {
        for (const Value &item : value.list) {
            if (item.type == Value::Type::Link) { openLinkTarget(item.text); break; }
        }
    }
}

void BaseView::onHeaderSortChanged(int column, Qt::SortOrder order)
{
    if (!m_tableModel)
        return;
    m_tableModel->sort(column, order);
    if (m_path.isEmpty() || !view())
        return; // a ```base``` block (no path of its own) is never rewritten
    const QString propertyReference = [&]() -> QString {
        for (const QString &ref : view()->order) {
            if (m_tableModel->columnOf(ref) == column)
                return ref;
        }
        return QString();
    }();
    if (propertyReference.isEmpty())
        return;
    const SortDirection direction = order == Qt::AscendingOrder ? SortDirection::Ascending : SortDirection::Descending;
    const std::optional<QString> rewritten = BaseFile::rewriteViewSort(m_yamlText, m_currentView, propertyReference, direction);
    if (!rewritten)
        return; // this .base file's shape can't be edited safely; the on-screen sort still applies
    if (QFileInfo(m_path).isSymLink())
        return; // a link is not written through, so the on-screen sort is all there is
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QByteArray bytes = rewritten->toUtf8();
    if (file.write(bytes) == bytes.size() && file.commit())
        m_yamlText = *rewritten;
}

void BaseView::onCellEdited(const QString &)
{
    // The write already refreshed the index; rerunning the view picks up
    // whatever the edit changed - including a sort/filter that now excludes
    // the edited row.
    openCurrentView();
}

QStringList BaseView::warnings() const
{
    return m_query ? m_query->warnings() : QStringList();
}

void BaseView::rebuildCards()
{
    QLayoutItem *item = nullptr;
    while ((item = m_cardsLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    if (!m_query)
        return;

    constexpr int cardWidth = 180;
    const int columns = std::max(1, m_cardsArea->viewport()->width() / cardWidth);
    int column = 0, row = 0;
    for (int i = 0; i < int(m_query->resultRows().size()); ++i) {
        m_cardsLayout->addWidget(makeCard(i), row, column);
        if (++column >= columns) { column = 0; ++row; }
    }
}

QWidget *BaseView::makeCard(int position)
{
    const qsizetype rowIndex = m_query->resultRows().at(position);
    const Row &sourceRow = m_query->rows().at(rowIndex);
    const ViewConfig *v = view();

    auto *frame = new QFrame;
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setFixedWidth(170);
    auto *frameLayout = new QVBoxLayout(frame);

    if (v && !v->imageProperty.isEmpty()) {
        const QString ref = m_query->valueOf(rowIndex, v->imageProperty).toDisplayString();
        auto *cover = new QLabel;
        cover->setFixedSize(150, 90);
        cover->setAlignment(Qt::AlignCenter);
        if (QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{3,8}$")).match(ref).hasMatch()) {
            cover->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(ref));
        } else if (ref.startsWith(QLatin1String("http://")) || ref.startsWith(QLatin1String("https://"))) {
            cover->setText(tr("(remote image)")); // never fetched, by design
        } else if (!ref.isEmpty()) {
            QString target = ref;
            static const QRegularExpression wikilink(
                QStringLiteral("^!?\\[\\[([^\\]|]+)(?:\\|[^\\]]+)?\\]\\]$"));
            if (const auto m = wikilink.match(target); m.hasMatch())
                target = m.captured(1).trimmed();
            const qsizetype hash = target.indexOf(QLatin1Char('#'));
            if (hash >= 0) target.truncate(hash);
            QString absolutePath = m_linkResolver ? m_linkResolver(target) : QString();
            if (absolutePath.isEmpty() && m_vault)
                absolutePath = m_vault->absolute(target); // no resolver: assume an already vault-relative path
            if (!absolutePath.isEmpty())
                loadCardImage(cover, absolutePath, cover->size());
        }
        frameLayout->addWidget(cover);
    }

    auto *name = new QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(sourceRow.noteName.toHtmlEscaped()));
    name->setTextFormat(Qt::RichText);
    name->setOpenExternalLinks(false);
    connect(name, &QLabel::linkActivated, this,
           [this, path = sourceRow.notePath] { emit noteActivated(path); });
    frameLayout->addWidget(name);

    if (v) {
        for (const QString &ref : v->order) {
            if (ref == v->imageProperty || ref == QLatin1String("file.name"))
                continue;
            const Value value = m_query->valueOf(rowIndex, ref);
            if (value.isEmpty())
                continue;
            frameLayout->addWidget(makeCardField(position, ref, value));
        }
    }
    frameLayout->addStretch(1);
    return frame;
}

QLabel *BaseView::makeChipLabel(const Value &item)
{
    auto *chip = new QLabel;
    if (item.type == Value::Type::Link) {
        chip->setTextFormat(Qt::RichText);
        chip->setText(QStringLiteral("<a href=\"#\" style=\"text-decoration:none;\">%1</a>")
                          .arg(item.toDisplayString().toHtmlEscaped()));
        chip->setOpenExternalLinks(false);
        connect(chip, &QLabel::linkActivated, this, [this, target = item.text] { openLinkTarget(target); });
    } else {
        chip->setText(item.toDisplayString().toHtmlEscaped());
    }
    chip->setStyleSheet(QStringLiteral("background:palette(button);border-radius:8px;padding:1px 8px;"));
    return chip;
}

QWidget *BaseView::makeCardField(int position, const QString &reference, const Value &value)
{
    const bool isNoteRef = !reference.startsWith(QLatin1String("file."))
                          && !reference.startsWith(QLatin1String("formula."));
    const int column = m_tableModel ? m_tableModel->columnOf(reference) : -1;
    const bool canEdit = isNoteRef && m_index && m_vault && column >= 0;

    if (value.type == Value::Type::Bool) {
        auto *box = new QCheckBox(m_file->displayNameFor(reference));
        box->setChecked(value.boolean);
        box->setEnabled(canEdit);
        if (canEdit) {
            connect(box, &QCheckBox::toggled, this, [this, position, column](bool checked) {
                m_tableModel->setData(m_tableModel->index(position, column),
                                      checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
            });
        }
        return box;
    }

    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *caption = new QLabel(m_file->displayNameFor(reference) + QLatin1Char(':'));
    caption->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(caption);

    if (value.type == Value::Type::List) {
        auto *chipRow = new QWidget;
        auto *chipLayout = new QHBoxLayout(chipRow);
        chipLayout->setContentsMargins(0, 0, 0, 0);
        chipLayout->setSpacing(4);
        for (const Value &item : value.list)
            chipLayout->addWidget(makeChipLabel(item));
        chipLayout->addStretch(1);
        layout->addWidget(chipRow);
        if (canEdit) {
            // Chips are the read view; a comma-joined line edits the whole
            // list at once (BaseTableModel::setData's own list path) - the
            // same corner cut as the table's own list cell editor.
            auto *edit = new QLineEdit(value.toDisplayString());
            edit->setPlaceholderText(tr("comma-separated"));
            connect(edit, &QLineEdit::editingFinished, this, [this, position, column, edit] {
                m_tableModel->setData(m_tableModel->index(position, column), edit->text());
            });
            layout->addWidget(edit);
        }
        return container;
    }

    if (value.type == Value::Type::Link) {
        // Not text-editable here: retyping "[[Note]]" syntax by hand in a
        // card is not worth it when the table's own cell already can.
        layout->addWidget(makeChipLabel(value));
        return container;
    }

    if (canEdit) {
        auto *edit = new QLineEdit(value.toDisplayString());
        connect(edit, &QLineEdit::editingFinished, this, [this, position, column, edit] {
            m_tableModel->setData(m_tableModel->index(position, column), edit->text());
        });
        layout->addWidget(edit);
        return container;
    }

    auto *text = new QLabel(value.toDisplayString());
    text->setWordWrap(true);
    layout->addWidget(text);
    return container;
}

void BaseView::openLinkTarget(const QString &rawTarget)
{
    QString target = rawTarget.trimmed();
    if (target.isEmpty() || !m_vault)
        return;
    const qsizetype hash = target.indexOf(QLatin1Char('#'));
    if (hash >= 0) target.truncate(hash);
    if (target.isEmpty())
        return;

    QString absolutePath = m_linkResolver ? m_linkResolver(target) : QString();
    if (absolutePath.isEmpty()) {
        // No resolver wired (a test, or a caller that never called
        // setLinkResolver): treat the target as already a vault-relative
        // note path, the common case for the simplest vaults - not a
        // substitute for the real fuzzy resolver MainWindow has.
        QString guess = target;
        if (!guess.contains(QLatin1Char('.')))
            guess += QStringLiteral(".md");
        const QString candidate = m_vault->absolute(guess);
        if (QFileInfo::exists(candidate))
            absolutePath = candidate;
    }
    if (absolutePath.isEmpty())
        return;
    const QString relative = m_vault->relative(absolutePath);
    if (!relative.isEmpty())
        emit noteActivated(relative);
}

void BaseView::loadCardImage(QLabel *target, const QString &absolutePath, const QSize &size)
{
    const QString cacheKey = absolutePath + QLatin1Char('@') + QString::number(size.width())
                            + QLatin1Char('x') + QString::number(size.height());
    if (QPixmap *cached = m_imageCache.object(cacheKey)) {
        target->setPixmap(*cached);
        return;
    }
    auto *watcher = new QFutureWatcher<QImage>(target);
    connect(watcher, &QFutureWatcher<QImage>::finished, target, [target, watcher, cacheKey, this] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (image.isNull())
            return;
        // Set from a stack copy before inserting into the cache: QCache may
        // delete the object it was given immediately if it does not fit,
        // and setPixmap() after that would read freed memory.
        const QPixmap pixmap = QPixmap::fromImage(image);
        target->setPixmap(pixmap);
        m_imageCache.insert(cacheKey, new QPixmap(pixmap));
    });
    watcher->setFuture(QtConcurrent::run([absolutePath, size] {
        QImage image;
        if (!image.load(absolutePath))
            return QImage();
        return image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }));
}

} // namespace noteAppC::bases
