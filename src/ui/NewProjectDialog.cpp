#include "ui/NewProjectDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace pl {

NewProjectDialog::NewProjectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Reference Tree"));
    setModal(true);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("e.g. Orchids of Victoria"));

    m_taxon = new QLineEdit(this);
    m_taxon->setPlaceholderText(tr("e.g. Orchidaceae"));

    m_rank = new QComboBox(this);
    m_rank->addItem(tr("(any rank)"), QString());
    for (const QString &rank : {QStringLiteral("kingdom"), QStringLiteral("phylum"),
                                QStringLiteral("class"), QStringLiteral("order"),
                                QStringLiteral("family"), QStringLiteral("subfamily"),
                                QStringLiteral("tribe"), QStringLiteral("genus"),
                                QStringLiteral("species")}) {
        m_rank->addItem(rank, rank);
    }

    m_place = new QLineEdit(this);
    m_place->setPlaceholderText(tr("e.g. Victoria, AU — leave blank for worldwide"));

    auto *form = new QFormLayout;
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("Root &taxon:"), m_taxon);
    form->addRow(tr("&Rank:"), m_rank);
    form->addRow(tr("R&egion:"), m_place);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Build"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(
        tr("PhotoLife will fetch the taxonomy and the region's species list from "
           "iNaturalist. This can take a minute for a large family."),
        this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewProjectDialog::updateOkState);
    connect(m_taxon, &QLineEdit::textChanged, this, &NewProjectDialog::updateOkState);
    updateOkState();
}

void NewProjectDialog::updateOkState()
{
    const bool valid = !m_name->text().trimmed().isEmpty()
                       && !m_taxon->text().trimmed().isEmpty();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

taxonomy::ProjectBuilder::Request NewProjectDialog::request() const
{
    taxonomy::ProjectBuilder::Request r;
    r.projectName = m_name->text().trimmed();
    r.taxonQuery = m_taxon->text().trimmed();
    r.rank = m_rank->currentData().toString();
    r.placeQuery = m_place->text().trimmed();
    return r;
}

} // namespace pl
