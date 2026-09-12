#include "ui/TaxonConfirmDialog.h"

#include "net/INatClient.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace pl {

TaxonConfirmDialog::TaxonConfirmDialog(pl::net::INatClient &inat, const QString &initialQuery,
                                       const QString &initialRank, QWidget *parent)
    : QDialog(parent), m_inat(inat)
{
    setWindowTitle(tr("Confirm Root Taxon"));
    setModal(true);
    resize(420, 480);

    m_query = new QLineEdit(initialQuery, this);
    m_query->setPlaceholderText(tr("Search iNaturalist taxa…"));
    connect(m_query, &QLineEdit::returnPressed, this, &TaxonConfirmDialog::runSearch);

    m_rank = new QComboBox(this);
    m_rank->addItem(tr("(any rank)"), QString());
    for (const QString &rank : {QStringLiteral("kingdom"), QStringLiteral("phylum"),
                                QStringLiteral("class"), QStringLiteral("order"),
                                QStringLiteral("family"), QStringLiteral("subfamily"),
                                QStringLiteral("tribe"), QStringLiteral("genus"),
                                QStringLiteral("species")}) {
        m_rank->addItem(rank, rank);
    }
    const int rankIndex = m_rank->findData(initialRank);
    m_rank->setCurrentIndex(rankIndex >= 0 ? rankIndex : 0);
    connect(m_rank, &QComboBox::currentIndexChanged, this, &TaxonConfirmDialog::runSearch);

    auto *searchButton = new QPushButton(tr("Search"), this);
    connect(searchButton, &QPushButton::clicked, this, &TaxonConfirmDialog::runSearch);

    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_query, 1);
    searchRow->addWidget(m_rank);
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
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(searchRow);
    layout->addWidget(m_status);
    layout->addWidget(m_candidates, 1);
    layout->addWidget(m_buttons);

    runSearch();
}

std::optional<pl::taxonomy::Taxon> TaxonConfirmDialog::chosenTaxon() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    if (!item)
        return std::nullopt;
    return m_taxa.at(item->data(Qt::UserRole).toInt());
}

void TaxonConfirmDialog::setBusy(bool busy)
{
    m_query->setEnabled(!busy);
    m_rank->setEnabled(!busy);
    m_candidates->setEnabled(!busy);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy && m_candidates->currentRow() >= 0);
}

void TaxonConfirmDialog::runSearch()
{
    const QString query = m_query->text().trimmed();
    const QString rank = m_rank->currentData().toString();
    m_taxa.clear();
    m_candidates->clear();

    if (query.isEmpty()) {
        m_status->setText(tr("Enter a taxon to search for."));
        return;
    }

    setBusy(true);
    m_status->setText(tr("Searching for \"%1\"…").arg(query));

    m_inat.searchTaxa(query, rank, [this, query](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
        setBusy(false);
        if (!out.ok()) {
            m_status->setText(tr("Search failed: %1").arg(out.error));
            populateCandidates({});
            return;
        }
        if (out.value.isEmpty())
            m_status->setText(tr("No taxon matched \"%1\". Try a different name or rank.")
                                  .arg(query));
        else
            m_status->setText(tr("%1 taxon/taxa matched \"%2\".").arg(out.value.size()).arg(query));
        populateCandidates(out.value);
    });
}

void TaxonConfirmDialog::populateCandidates(const QList<pl::taxonomy::Taxon> &candidates)
{
    m_taxa = candidates;

    for (int i = 0; i < m_taxa.size(); ++i) {
        const pl::taxonomy::Taxon &t = m_taxa.at(i);
        QString label = t.name;
        if (!t.commonName.isEmpty() && t.commonName != t.name)
            label += QStringLiteral("  ·  %1").arg(t.commonName);
        label += QStringLiteral("   [%1]").arg(t.rank);
        if (!t.isActive)
            label += QStringLiteral("   (inactive/synonym)");

        auto *item = new QListWidgetItem(label, m_candidates);
        item->setData(Qt::UserRole, i);
    }

    if (!m_taxa.isEmpty()) {
        int defaultRow = 0;
        for (int i = 0; i < m_taxa.size(); ++i) {
            if (m_taxa.at(i).isActive) {
                defaultRow = i;
                break;
            }
        }
        m_candidates->setCurrentRow(defaultRow);
    }
}

} // namespace pl
