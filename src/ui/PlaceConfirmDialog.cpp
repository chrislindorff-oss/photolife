#include "ui/PlaceConfirmDialog.h"

#include "net/INatClient.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace pl {

namespace {
constexpr int kWorldwideRow = -1;
}

PlaceConfirmDialog::PlaceConfirmDialog(pl::net::INatClient &inat, const QString &initialQuery,
                                       QWidget *parent)
    : QDialog(parent), m_inat(inat)
{
    setWindowTitle(tr("Confirm Region"));
    setModal(true);
    resize(420, 480);

    m_query = new QLineEdit(initialQuery, this);
    m_query->setPlaceholderText(tr("Search iNaturalist places…"));
    connect(m_query, &QLineEdit::returnPressed, this, &PlaceConfirmDialog::runSearch);

    auto *searchButton = new QPushButton(tr("Search"), this);
    connect(searchButton, &QPushButton::clicked, this, &PlaceConfirmDialog::runSearch);

    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_query, 1);
    searchRow->addWidget(searchButton);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_candidates = new QListWidget(this);
    connect(m_candidates, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_candidates, &QListWidget::currentRowChanged, this, [this](int row) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(row >= 0);
    });

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(searchRow);
    layout->addWidget(m_status);
    layout->addWidget(m_candidates, 1);
    layout->addWidget(m_buttons);

    runSearch();
}

std::optional<pl::taxonomy::Place> PlaceConfirmDialog::chosenPlace() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    if (!item)
        return std::nullopt;
    const int index = item->data(Qt::UserRole).toInt();
    if (index == kWorldwideRow)
        return std::nullopt;
    return m_places.at(index);
}

void PlaceConfirmDialog::setBusy(bool busy)
{
    m_query->setEnabled(!busy);
    m_candidates->setEnabled(!busy);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy && m_candidates->currentRow() >= 0);
}

void PlaceConfirmDialog::runSearch()
{
    const QString query = m_query->text().trimmed();
    m_places.clear();
    m_candidates->clear();

    if (query.isEmpty()) {
        populateCandidates({});
        m_status->setText(tr("Enter a region to search for, or build without a place filter."));
        return;
    }

    setBusy(true);
    m_status->setText(tr("Searching for \"%1\"…").arg(query));

    m_inat.resolvePlaces(query, [this, query](pl::net::Outcome<QList<pl::taxonomy::Place>> out) {
        setBusy(false);
        if (!out.ok()) {
            m_status->setText(tr("Search failed: %1").arg(out.error));
            populateCandidates({});
            return;
        }
        if (out.value.isEmpty())
            m_status->setText(tr("No region matched \"%1\". You can try another spelling, "
                                 "or build without a place filter.").arg(query));
        else
            m_status->setText(tr("%1 region(s) matched \"%2\".").arg(out.value.size()).arg(query));
        populateCandidates(out.value);
    });
}

void PlaceConfirmDialog::populateCandidates(const QList<pl::taxonomy::Place> &candidates)
{
    m_places = candidates;

    auto *worldwide = new QListWidgetItem(tr("No region filter (worldwide)"), m_candidates);
    worldwide->setData(Qt::UserRole, kWorldwideRow);

    for (int i = 0; i < m_places.size(); ++i) {
        const pl::taxonomy::Place &p = m_places.at(i);
        QString label = p.displayName.isEmpty() ? p.name : p.displayName;
        if (p.adminLevel)
            label += QStringLiteral("   [admin level %1]").arg(*p.adminLevel);
        auto *item = new QListWidgetItem(label, m_candidates);
        item->setData(Qt::UserRole, i);
    }

    m_candidates->setCurrentRow(m_places.isEmpty() ? 0 : 1);
}

} // namespace pl
