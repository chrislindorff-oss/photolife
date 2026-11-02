#include "ui/TreeChangesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pl {
namespace {

enum Column { TaxonColumn, PhotosColumn, ChangeColumn, ActionColumn };

QString nameOf(const QString &name, const QString &common)
{
    return common.isEmpty() ? name : QStringLiteral("%1 · %2").arg(name, common);
}

} // namespace

TreeChangesDialog::TreeChangesDialog(const QList<taxonomy::TreeChange> &changes, const QString &treeName,
                                     const QString &placeName, QWidget *parent)
    : QDialog(parent), m_changes(changes)
{
    setWindowTitle(tr("Tree Changes on iNaturalist"));
    const QString where = placeName.isEmpty() ? tr("anywhere") : placeName;
    auto *intro = new QLabel(
        tr("Refreshing <b>%1</b> found these taxa changed on iNaturalist, or no longer listed. "
           "Nothing changes until you choose what to do and Apply; <i>Ask again later</i> leaves a "
           "taxon as it is until the next refresh.")
            .arg(treeName.toHtmlEscaped()),
        this);
    intro->setWordWrap(true);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Taxon"), tr("Photos"), tr("What changed"), tr("Do")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->header()->setSectionResizeMode(TaxonColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(PhotosColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(ChangeColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(ActionColumn, QHeaderView::ResizeToContents);

    const QStringList headings = {tr("Renamed"), tr("Split"), tr("Retired"),
                                  tr("No longer recorded in %1").arg(where),
                                  tr("Subspecies with no records in %1").arg(where), tr("Empty groups")};
    int lastKind = -1;
    for (const taxonomy::TreeChange &c : std::as_const(m_changes)) {
        if (int(c.kind) != lastKind) {
            lastKind = int(c.kind);
            auto *heading = new QTreeWidgetItem(m_tree, {headings.value(lastKind)});
            QFont bold = heading->font(TaxonColumn);
            bold.setBold(true);
            heading->setFont(TaxonColumn, bold);
            heading->setFirstColumnSpanned(true);
        }
        auto *row = new QTreeWidgetItem(m_tree);
        row->setText(TaxonColumn, nameOf(c.taxon.name, c.taxon.commonName)
                                      + (c.taxon.rank == QLatin1String("species") || c.taxon.rank.isEmpty()
                                             ? QString()
                                             : QStringLiteral(" (%1)").arg(c.taxon.rank)));
        row->setText(PhotosColumn, c.taxon.photos > 0 ? QString::number(c.taxon.photos) : QStringLiteral("–"));
        row->setTextAlignment(PhotosColumn, Qt::AlignCenter);

        QString what;
        switch (c.kind) {
        case taxonomy::TreeChange::Renamed:
            what = tr("now %1").arg(nameOf(c.replacements.first().name, c.replacements.first().commonName));
            break;
        case taxonomy::TreeChange::Split: {
            QStringList names;
            for (const taxonomy::Taxon &t : c.replacements)
                names << t.name;
            what = tr("split into %1").arg(names.join(QStringLiteral(", ")));
            break;
        }
        case taxonomy::TreeChange::Retired:
            what = tr("no longer used on iNaturalist");
            break;
        case taxonomy::TreeChange::NotRecorded:
            what = tr("no verifiable observations there now");
            break;
        case taxonomy::TreeChange::SubspeciesNotRecorded:
            what = tr("none named to this subspecies there");
            row->setToolTip(ChangeColumn, tr("Most observations are named only to species, so this "
                                             "subspecies may still live there — check before pruning."));
            break;
        case taxonomy::TreeChange::EmptyGroup:
            what = tr("nothing beneath it in the tree");
            break;
        }
        row->setText(ChangeColumn, what);
        if (row->toolTip(ChangeColumn).isEmpty())
            row->setToolTip(ChangeColumn, what);

        auto *action = new QComboBox(m_tree);
        action->addItem(tr("Ask again later"), int(Action::Later));
        if (c.kind != taxonomy::TreeChange::Renamed && c.kind != taxonomy::TreeChange::Split)
            action->addItem(tr("Keep"), int(Action::Keep));
        action->addItem(c.kind == taxonomy::TreeChange::EmptyGroup ? tr("Prune (with anything filed under it)")
                                                                    : tr("Prune"),
                        int(Action::Prune));
        for (int i = 0; i < c.replacements.size(); ++i)
            action->addItem(c.taxon.photos > 0 ? tr("Move photos to %1").arg(c.replacements.at(i).name)
                                               : tr("Use %1").arg(c.replacements.at(i).name),
                            int(Action::Move) + 16 * i);
        if (c.kind == taxonomy::TreeChange::Renamed)
            action->setCurrentIndex(action->count() - 1);
        action->setToolTip(tr("Prune takes it out of this tree (its photos stay in the library, and show "
                              "under Photos not in any tree); Move re-points its photos and learned names "
                              "to the new name, adds that to the tree, and prunes the old one."));
        m_tree->setItemWidget(row, ActionColumn, action);
        m_actions << action;
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    // Subspecies can be many (every one of each species, from anywhere): the
    // ones without photos of yours, all at once.
    int bare = 0;
    for (const taxonomy::TreeChange &c : std::as_const(m_changes))
        bare += c.kind == taxonomy::TreeChange::SubspeciesNotRecorded && c.taxon.photos == 0 ? 1 : 0;
    if (bare > 0) {
        QPushButton *pruneBare = buttons->addButton(tr("Prune %n Subspecies Without Photos", nullptr, bare),
                                                    QDialogButtonBox::ActionRole);
        pruneBare->setToolTip(tr("Sets every subspecies listed here that you have no photos of to Prune. "
                                 "Nothing happens until Apply."));
        connect(pruneBare, &QPushButton::clicked, this, [this] {
            for (int i = 0; i < m_changes.size(); ++i) {
                const taxonomy::TreeChange &c = m_changes.at(i);
                if (c.kind != taxonomy::TreeChange::SubspeciesNotRecorded || c.taxon.photos > 0)
                    continue;
                QComboBox *action = m_actions.at(i);
                action->setCurrentIndex(action->findData(int(Action::Prune)));
            }
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_tree, 1);
    layout->addWidget(buttons);
    resize(900, 560);
}

QList<TreeChangesDialog::Choice> TreeChangesDialog::choices() const
{
    QList<Choice> out;
    for (int i = 0; i < m_changes.size(); ++i) {
        Choice choice;
        choice.change = m_changes.at(i);
        const int data = m_actions.at(i)->currentData().toInt();
        if (data >= int(Action::Move)) {
            choice.action = Action::Move;
            choice.moveTo = choice.change.replacements.value((data - int(Action::Move)) / 16);
        } else {
            choice.action = Action(data);
        }
        out << choice;
    }
    return out;
}

} // namespace pl
