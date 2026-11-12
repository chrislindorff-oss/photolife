#include "ui/DuplicatesDialog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

DuplicatesDialog::DuplicatesDialog(const QList<catalogue::DuplicateGroup> &groups, int stale, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Duplicate Photos"));
    const QLocale locale;
    qint64 spare = 0;
    int copies = 0;
    for (const auto &g : groups) {
        spare += g.spareBytes();
        copies += int(g.paths.size()) - 1;
    }
    auto *summary = new QLabel(this);
    summary->setWordWrap(true);
    summary->setText(groups.isEmpty()
                         ? tr("No duplicates: every file in the library is the only one of its kind.")
                         : tr("%n file(s) in the library have an identical copy somewhere else in it — %1 spare "
                              "copies, %2 that keeping one of each would free.",
                              nullptr, int(groups.size()))
                               .arg(copies)
                               .arg(locale.formattedDataSize(spare)));
    if (stale > 0)
        summary->setText(summary->text() + QStringLiteral("\n\n")
                         + tr("%n more file(s) the catalogue lists aren't on disk any more (renamed or deleted "
                              "outside PhotoLife), so they're left out. Library → Remove Photos With Missing "
                              "Files… tidies the catalogue.",
                              nullptr, stale));
    auto *note = new QLabel(tr("PhotoLife never deletes photos. To remove a copy, show it in the file manager and "
                               "delete it there; Rescan Library then catches up. Check the copies are the same "
                               "photo first — a RAW and its JPEG are different files and never appear here."),
                            this);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("muted"));

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("File"), tr("Folder")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setStretchLastSection(true);
    for (const auto &g : groups) {
        auto *top = new QTreeWidgetItem(m_tree);
        top->setText(0, tr("%1 — %n copies, %2 each", nullptr, int(g.paths.size()))
                            .arg(QFileInfo(g.paths.first()).fileName(), locale.formattedDataSize(g.size)));
        top->setFirstColumnSpanned(true);
        QFont bold = top->font(0);
        bold.setBold(true);
        top->setFont(0, bold);
        for (const QString &path : g.paths) {
            auto *child = new QTreeWidgetItem(top);
            child->setText(0, QFileInfo(path).fileName());
            child->setText(1, QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
            child->setData(0, Qt::UserRole, path);
            child->setToolTip(0, QDir::toNativeSeparators(path));
            child->setToolTip(1, QDir::toNativeSeparators(path));
        }
    }
    m_tree->expandAll();
    m_tree->resizeColumnToContents(0);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *, int) { showSelected(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *show = buttons->addButton(tr("Show in File Manager"), QDialogButtonBox::ActionRole);
    show->setEnabled(false);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [show](QTreeWidgetItem *item) {
        show->setEnabled(item && !item->data(0, Qt::UserRole).toString().isEmpty());
    });
    connect(show, &QPushButton::clicked, this, &DuplicatesDialog::showSelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(summary);
    layout->addWidget(m_tree, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(900, 600);
}

void DuplicatesDialog::showSelected()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

} // namespace pl
