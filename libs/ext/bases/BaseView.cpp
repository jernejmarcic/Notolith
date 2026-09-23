#include "BaseView.h"

#include "BaseQuery.h"
#include "BaseTableModel.h"
#include "Index.h"
#include "Vault.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

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
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionsMovable(false);
    m_table->verticalHeader()->setVisible(false);
    connect(m_table, &QTableView::doubleClicked, this, &BaseView::onTableActivated);
    connect(m_table, &QTableView::clicked, this, [this](const QModelIndex &index) {
        if (m_tableModel && m_tableModel->isNameColumn(index.column()))
            onTableActivated(index);
    });
    m_stack->addWidget(m_table);

    m_cardsArea = new QScrollArea(this);
    m_cardsArea->setWidgetResizable(true);
    m_cardsContent = new QWidget;
    m_cardsLayout = new QGridLayout(m_cardsContent);
    m_cardsArea->setWidget(m_cardsContent);
    m_stack->addWidget(m_cardsArea);
}

BaseView::~BaseView() = default;

void BaseView::setIndex(Index *index) { m_index = index; }
void BaseView::setVault(const Vault *vault) { m_vault = vault; }

void BaseView::clear()
{
    m_file.reset();
    m_query.reset();
    m_table->setModel(nullptr);
    delete m_tableModel;
    m_tableModel = nullptr;
    rebuildCards();
    m_viewSwitcher->blockSignals(true);
    m_viewSwitcher->clear();
    m_viewSwitcher->blockSignals(false);
    m_status->clear();
    m_lastError.clear();
}

bool BaseView::open(const QString &absolutePath, const QString &yamlText)
{
    Q_UNUSED(absolutePath); // this module does no file I/O of its own beyond an edit's own note
    clear();
    QString error;
    auto parsed = BaseFile::parse(yamlText, &error);
    if (!parsed) {
        m_lastError = error;
        return false;
    }
    m_file = std::move(parsed);
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
            m_viewSwitcher->addItem(v.name.isEmpty() ? tr("View %1").arg(++i) : v.name);
            ++i;
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

    QList<Row> rows;
    for (const Index::BaseNote &note : m_index->allNotesForBases())
        rows << rowFromNote(note);

    m_query = std::make_unique<BaseQuery>(*m_file, std::move(rows), m_vault);
    QString error;
    if (!m_query->openView(m_currentView, &error)) {
        m_status->setText(error);
        m_query.reset();
        return;
    }

    delete m_tableModel;
    m_tableModel = new BaseTableModel(m_query.get(), m_index, m_vault, this);
    connect(m_tableModel, &BaseTableModel::noteEdited, this, &BaseView::onCellEdited);
    m_table->setModel(m_tableModel);

    const bool cards = view() && view()->type.compare(QLatin1String("cards"), Qt::CaseInsensitive) == 0;
    m_stack->setCurrentWidget(cards ? static_cast<QWidget *>(m_cardsArea) : m_table);
    if (cards)
        rebuildCards();

    const QStringList &warn = m_query->warnings();
    m_status->setText(warn.isEmpty()
        ? tr("%1 notes").arg(m_query->resultRows().size())
        : tr("%1 notes - %2 warning(s)").arg(m_query->resultRows().size()).arg(warn.size()));
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
    for (qsizetype i = 0; i < m_query->resultRows().size(); ++i) {
        m_cardsLayout->addWidget(makeCard(m_query->resultRows().at(i)), row, column);
        if (++column >= columns) { column = 0; ++row; }
    }
}

QWidget *BaseView::makeCard(qsizetype rowIndex)
{
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
            cover->setText(tr("(remote image)"));
        } else if (!ref.isEmpty() && m_vault) {
            const QPixmap pixmap(m_vault->absolute(ref));
            if (!pixmap.isNull())
                cover->setPixmap(pixmap.scaled(cover->size(), Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation));
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
            const QString text = m_query->valueOf(rowIndex, ref).toDisplayString();
            if (text.isEmpty())
                continue;
            auto *field = new QLabel(QStringLiteral("%1: %2").arg(m_file->displayNameFor(ref), text));
            field->setWordWrap(true);
            frameLayout->addWidget(field);
        }
    }
    frameLayout->addStretch(1);
    return frame;
}

} // namespace noteAppC::bases
