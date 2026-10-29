#include "ui/InatScopeWidgets.h"

#include "ui/TaxonConfirmDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

namespace pl {

ReferenceTreeCombo::ReferenceTreeCombo(const QString &allTreesLabel, QWidget *parent)
    : QComboBox(parent), m_allTreesLabel(allTreesLabel)
{
    if (!m_allTreesLabel.isEmpty())
        addItem(m_allTreesLabel, 0);
    connect(this, &QComboBox::activated, this, [this] { m_chosenByUser = true; });
}

void ReferenceTreeCombo::setTrees(const QList<std::pair<int, QString>> &trees,
                                  int activeProjectId)
{
    const int previous = m_chosenByUser ? projectId() : activeProjectId;
    {
        QSignalBlocker block(this);
        clear();
        if (!m_allTreesLabel.isEmpty())
            addItem(m_allTreesLabel, 0);
        for (const auto &[id, name] : trees)
            addItem(name, id);
        int restore = findData(previous);
        if (restore < 0) {
            m_chosenByUser = false;   // the user's tree is gone -- follow the dock again
            restore = findData(activeProjectId);
        }
        setCurrentIndex(restore >= 0 ? restore : 0);
    }
    emit currentIndexChanged(currentIndex());
}

void ReferenceTreeCombo::followActiveTree(int projectId)
{
    if (m_chosenByUser)
        return;
    const int index = findData(projectId);
    if (index >= 0)
        setCurrentIndex(index);
}

int ReferenceTreeCombo::projectId() const
{
    return currentIndex() >= 0 ? currentData().toInt() : -1;
}

QList<int> ReferenceTreeCombo::allProjectIds() const
{
    QList<int> ids;
    for (int i = 0; i < count(); ++i) {
        if (const int id = itemData(i).toInt(); id > 0)
            ids.append(id);
    }
    return ids;
}

TaxonLimitRow::TaxonLimitRow(net::INatClient &inat, QWidget *parent)
    : QWidget(parent), m_inat(inat)
{
    m_text = new QLabel(this);
    m_text->setTextFormat(Qt::RichText);

    auto *chooseButton = new QPushButton(tr("Choose Taxon…"), this);
    chooseButton->setToolTip(
        tr("Search iNaturalist for a group — a family, genus, order and so on — and limit "
           "this tool to it and everything beneath it. Combines with the tree choice: only "
           "the tree's taxa within that group are considered."));
    connect(chooseButton, &QPushButton::clicked, this, &TaxonLimitRow::chooseTaxon);

    m_useTreeSelectionButton = new QPushButton(tr("Use Tree Selection"), this);
    connect(m_useTreeSelectionButton, &QPushButton::clicked, this,
            [this] { setTaxon(m_treeSelectionId, m_treeSelectionName); });

    m_clearButton = new QPushButton(tr("All Taxa"), this);
    m_clearButton->setToolTip(tr("Remove the taxon limit."));
    connect(m_clearButton, &QPushButton::clicked, this, [this] { setTaxon(0, QString()); });

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(tr("Limit to taxon:"), this));
    layout->addWidget(m_text, 1);
    layout->addWidget(chooseButton);
    layout->addWidget(m_useTreeSelectionButton);
    layout->addWidget(m_clearButton);

    setTaxon(0, QString());
    setTreeSelection(0, QString());
}

void TaxonLimitRow::setTaxon(qint64 inatId, const QString &label)
{
    const bool same = inatId == m_taxonId;
    m_taxonId = inatId;
    m_label = inatId > 0 ? label : QString();
    m_text->setText(inatId > 0 ? QStringLiteral("<b>%1</b>").arg(label.toHtmlEscaped())
                               : tr("<i>All taxa</i>"));
    updateButtons();
    if (!same)
        emit changed();
}

void TaxonLimitRow::setTreeSelection(qint64 inatId, const QString &name)
{
    m_treeSelectionId = inatId;
    m_treeSelectionName = name;
    m_useTreeSelectionButton->setToolTip(
        inatId > 0 ? tr("Limit to \"%1\", the taxon selected in the Reference Trees panel.")
                         .arg(name)
                   : tr("Select a taxon in the Reference Trees panel first (F9 shows the panel)."));
    updateButtons();
}

void TaxonLimitRow::chooseTaxon()
{
    TaxonConfirmDialog dialog(m_inat, QString(), QString(), this, tr("Limit to Taxon"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto chosen = dialog.chosenTaxon();
    if (!chosen)
        return;
    setTaxon(chosen->inatId, chosen->commonName.isEmpty()
                                 ? chosen->name
                                 : tr("%1 (%2)").arg(chosen->name, chosen->commonName));
}

void TaxonLimitRow::updateButtons()
{
    m_clearButton->setEnabled(m_taxonId > 0);
    m_useTreeSelectionButton->setEnabled(m_treeSelectionId > 0
                                         && m_treeSelectionId != m_taxonId);
}

} // namespace pl
