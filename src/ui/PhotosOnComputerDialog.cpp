#include "ui/PhotosOnComputerDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace pl {

PhotosOnComputerDialog::PhotosOnComputerDialog(
    const QList<libsync::PhotoSelection::TreeStats> &trees, bool allTrees, const QList<int> &ticked,
    QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Photos on This Computer"));
    resize(520, 520);

    m_all = new QRadioButton(tr("Keep &every photo on this computer"), this);
    m_some = new QRadioButton(tr("Keep &only the photos of these reference trees:"), this);
    m_all->setChecked(allTrees);
    m_some->setChecked(!allTrees);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("trees"));
    for (const auto &tree : trees) {
        auto *item = new QListWidgetItem(
            tr("%1 — %Ln photo(s), %2", nullptr, tree.photos)
                .arg(tree.name, QLocale().formattedDataSize(tree.bytes)),
            m_list);
        item->setData(Qt::UserRole, tree.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(ticked.contains(tree.id) ? Qt::Checked : Qt::Unchecked);
    }

    auto *note = new QLabel(
        tr("Photos not yet matched, photos matched to taxa in no tree, and photos you've "
           "downloaded by opening them are always kept. The rest stay online: you can still "
           "browse them by thumbnail, and they download when you open or export them. Nothing "
           "is deleted from this computer without asking."),
        this);
    note->setWordWrap(true);
    note->setEnabled(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_all);
    layout->addWidget(m_some);
    layout->addWidget(m_list, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);

    connect(m_some, &QRadioButton::toggled, this, &PhotosOnComputerDialog::updateEnabled);
    updateEnabled();
}

void PhotosOnComputerDialog::updateEnabled()
{
    m_list->setEnabled(m_some->isChecked());
}

bool PhotosOnComputerDialog::allTrees() const
{
    return m_all->isChecked();
}

QList<int> PhotosOnComputerDialog::tickedTrees() const
{
    QList<int> out;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->checkState() == Qt::Checked)
            out << m_list->item(i)->data(Qt::UserRole).toInt();
    }
    return out;
}

} // namespace pl
