#include "ui/LibraryFoldersDialog.h"

#include "app/Application.h"
#include "db/Database.h"
#include "scan/CatalogueMaintenance.h"
#include "scan/LibraryWatcher.h"
#include "settings/Settings.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace pl {

LibraryFoldersDialog::LibraryFoldersDialog(Application &app, QWidget *parent)
    : QDialog(parent), m_app(app)
{
    setWindowTitle(tr("Manage Library Folders"));
    resize(640, 380);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {tr("Folder"), tr("Status"), tr("Catalogued Photos")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);

    auto *removeButton = new QPushButton(tr("&Remove Selected…"), this);
    connect(removeButton, &QPushButton::clicked, this, &LibraryFoldersDialog::removeSelected);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(this, &QDialog::finished, this, [this] {
        if (m_dirty)
            emit librariesChanged();
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("These are the folders PhotoLife is watching. Folders marked "
           "\"Missing\" no longer exist at that location -- remove one to "
           "either purge its catalogued photos or just stop watching it."),
        this));
    layout->addWidget(m_table, 1);
    layout->addWidget(removeButton);
    layout->addWidget(buttons);

    reload();
}

void LibraryFoldersDialog::reload()
{
    m_table->setRowCount(0);

    const QStringList roots = m_app.settings().watchedRoots();
    scan::CatalogueMaintenance maint(m_app.database().connectionName());

    for (const QString &root : roots) {
        const bool exists = QDir(root).exists();
        const int count = maint.captureCountUnderFolder(root);

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *pathItem = new QTableWidgetItem(root);
        pathItem->setData(Qt::UserRole, root);
        m_table->setItem(row, 0, pathItem);

        auto *statusItem = new QTableWidgetItem(exists ? tr("OK") : tr("Missing"));
        if (!exists)
            statusItem->setForeground(Qt::red);
        m_table->setItem(row, 1, statusItem);

        m_table->setItem(row, 2, new QTableWidgetItem(QString::number(count)));
    }
}

void LibraryFoldersDialog::removeSelected()
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;

    const QString root = m_table->item(rows.first().row(), 0)->data(Qt::UserRole).toString();
    const bool exists = QDir(root).exists();

    auto stopWatching = [this, &root] {
        QStringList roots = m_app.settings().watchedRoots();
        roots.removeAll(root);
        m_app.settings().setWatchedRoots(roots);
        m_app.libraryWatcher().setRoots(roots);
        m_dirty = true;
    };

    if (exists) {
        const auto reply = QMessageBox::question(
            this, tr("Remove Folder"),
            tr("Stop watching \"%1\"? Its catalogued photos are kept -- this "
               "only removes it from the library's list of watched folders.")
                .arg(root),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;

        stopWatching();
        reload();
        return;
    }

    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int captureCount = maint.captureCountUnderFolder(root);

    QMessageBox box(QMessageBox::Warning, tr("Remove Folder"),
                    tr("\"%1\" no longer exists -- it may have been deleted, "
                       "renamed, or is on a drive that isn't connected.\n\n"
                       "%n catalogued photo(s) are still on record from it.",
                       nullptr, captureCount)
                        .arg(root),
                    QMessageBox::NoButton, this);
    QPushButton *purgeButton = box.addButton(tr("Purge"), QMessageBox::DestructiveRole);
    QPushButton *retainButton = box.addButton(tr("Retain"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(retainButton);
    box.exec();

    if (box.clickedButton() == purgeButton) {
        const int removed = maint.purgeFolder(root);
        if (removed < 0) {
            QMessageBox::warning(this, tr("Remove Folder"),
                                 tr("Could not purge \"%1\": %2").arg(root, maint.error()));
            return;
        }
        stopWatching();
        reload();
    } else if (box.clickedButton() == retainButton) {
        stopWatching();
        reload();
    }
    // Cancel (or the box closed some other way): do nothing.
}

} // namespace pl
