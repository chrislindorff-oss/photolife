#include "ui/TaxonPickerDialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace pl {

TaxonPickerDialog::TaxonPickerDialog(QString connectionName, QWidget *parent)
    : QDialog(parent), m_finder(std::move(connectionName))
{
    setWindowTitle(tr("Choose a Taxon"));
    resize(420, 480);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search for a taxon…"));
    connect(m_search, &QLineEdit::textEdited, this, &TaxonPickerDialog::runSearch);

    m_candidates = new QListWidget(this);
    connect(m_candidates, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_search);
    layout->addWidget(m_candidates, 1);
    layout->addWidget(buttons);

    m_search->setFocus();
}

qint64 TaxonPickerDialog::chosenTaxonInatId() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void TaxonPickerDialog::runSearch(const QString &text)
{
    if (text.trimmed().size() < 3) {
        m_candidates->clear();
        return;
    }
    populateCandidates(m_finder.search(text));
}

void TaxonPickerDialog::populateCandidates(const QList<pl::match::TaxonCandidate> &candidates)
{
    m_candidates->clear();
    for (const auto &c : candidates) {
        QString label = c.name;
        if (!c.commonName.isEmpty() && c.commonName != c.name)
            label += QStringLiteral("  ·  %1").arg(c.commonName);
        label += QStringLiteral("   [%1]").arg(c.rank);

        auto *item = new QListWidgetItem(label, m_candidates);
        item->setData(Qt::UserRole, c.inatId);
    }
    if (m_candidates->count() > 0)
        m_candidates->setCurrentRow(0);
}

} // namespace pl
