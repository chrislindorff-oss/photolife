#include "ui/StaleMatchDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pl {

StaleMatchDialog::StaleMatchDialog(const QList<match::StaleDecision> &stale, QWidget *parent)
    : QDialog(parent), m_stale(stale)
{
    setWindowTitle(tr("Renamed Photos"));
    resize(760, 420);

    auto *intro = new QLabel(
        tr("%n photo(s) you identified yourself now have a file name pointing to a different "
           "taxon — usually because the file was renamed after you identified it. Match Library "
           "never changes your own identifications on its own.\n\n"
           "Checked photos will be re-associated with the taxon in their new name. Unchecked "
           "photos keep your identification, and won't be asked about again unless renamed "
           "once more.",
           nullptr, int(stale.size())),
        this);
    intro->setWordWrap(true);

    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setHeaderLabels({tr("Photo"), tr("You identified it as"), tr("Its name now says")});
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (const match::StaleDecision &s : stale) {
        auto *item = new QTreeWidgetItem(m_list, {s.baseName, s.decidedName, s.suggestedName});
        item->setCheckState(0, Qt::Checked);
        item->setToolTip(0, s.baseName);
    }

    auto *checkAll = new QPushButton(tr("Check All"), this);
    connect(checkAll, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    auto *checkNone = new QPushButton(tr("Check None"), this);
    connect(checkNone, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    auto *checkRow = new QHBoxLayout;
    checkRow->addWidget(checkAll);
    checkRow->addWidget(checkNone);
    checkRow->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *apply =
        buttons->addButton(tr("Re-associate Checked"), QDialogButtonBox::AcceptRole);
    apply->setDefault(true);
    apply->setToolTip(tr("Checked photos move to the taxon in their new name; unchecked photos "
                         "keep your identification."));
    QPushButton *later = buttons->addButton(tr("Decide Later"), QDialogButtonBox::RejectRole);
    later->setToolTip(tr("Change nothing now — you'll be asked again after the next Match "
                         "Library."));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addLayout(checkRow);
    layout->addWidget(buttons);
}

void StaleMatchDialog::setAllChecked(bool checked)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        m_list->topLevelItem(i)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
}

QList<match::StaleDecision> StaleMatchDialog::toReassociate() const
{
    QList<match::StaleDecision> out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        if (m_list->topLevelItem(i)->checkState(0) == Qt::Checked)
            out.append(m_stale.at(i));
    }
    return out;
}

QList<match::StaleDecision> StaleMatchDialog::toKeep() const
{
    QList<match::StaleDecision> out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        if (m_list->topLevelItem(i)->checkState(0) != Qt::Checked)
            out.append(m_stale.at(i));
    }
    return out;
}

} // namespace pl
