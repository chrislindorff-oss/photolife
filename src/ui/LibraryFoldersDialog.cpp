#include "ui/LibraryFoldersDialog.h"

#include "app/Application.h"
#include "db/Database.h"
#include "scan/CatalogueMaintenance.h"
#include "scan/LibraryWatcher.h"
#include "settings/Settings.h"
#include "scan/UnsortedFolders.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
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

    // Unsorted folders: unculled runs, one photo per run catalogued.
    m_unsorted = new QTableWidget(this);
    m_unsorted->setColumnCount(3);
    m_unsorted->setHorizontalHeaderLabels({tr("Unsorted folder"), tr("Runs catalogued"), tr("Other shots")});
    m_unsorted->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_unsorted->setSelectionMode(QAbstractItemView::SingleSelection);
    m_unsorted->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_unsorted->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_unsorted->verticalHeader()->setVisible(false);
    auto *markButton = new QPushButton(tr("Mark a Folder &Unsorted…"), this);
    markButton->setToolTip(tr("A folder of unculled runs of shots: PhotoLife keeps one photo of each run in the "
                              "catalogue (so its species count) and only remembers the others. Rescans and "
                              "matching get faster. No file is changed."));
    connect(markButton, &QPushButton::clicked, this, &LibraryFoldersDialog::markUnsorted);
    auto *sortedButton = new QPushButton(tr("Mark as &Sorted"), this);
    sortedButton->setToolTip(tr("Culled and sorted: every photo there is catalogued (and matched) again."));
    connect(sortedButton, &QPushButton::clicked, this, &LibraryFoldersDialog::markSorted);
    auto *unsortedButtons = new QHBoxLayout;
    unsortedButtons->addWidget(markButton);
    unsortedButtons->addWidget(sortedButton);
    unsortedButtons->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("These are the folders PhotoLife is watching. Folders marked "
           "\"Missing\" no longer exist at that location -- remove one to "
           "either purge its catalogued photos or just stop watching it."),
        this));
    layout->addWidget(m_table, 1);
    layout->addWidget(removeButton);
    auto *unsortedIntro = new QLabel(
        tr("<b>Unsorted folders</b> hold runs of shots you haven't culled yet. PhotoLife catalogues one photo "
           "of each run there (shots of the same subject up to 10 seconds apart), shown with \u00d7 and the "
           "run's size, so their species still count, and remembers the rest without cataloguing them. Mark "
           "a folder Sorted once you've culled it."),
        this);
    unsortedIntro->setWordWrap(true);
    layout->addWidget(unsortedIntro);
    layout->addWidget(m_unsorted, 1);
    layout->addLayout(unsortedButtons);
    layout->addWidget(buttons);
    resize(720, 560);

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

    m_unsorted->setRowCount(0);
    for (const scan::Unsorted::FolderStats &s : scan::Unsorted::stats(m_app.database().connectionName())) {
        const int row = m_unsorted->rowCount();
        m_unsorted->insertRow(row);
        auto *path = new QTableWidgetItem(QDir::toNativeSeparators(s.path));
        path->setData(Qt::UserRole, s.path);
        m_unsorted->setItem(row, 0, path);
        m_unsorted->setItem(row, 1, new QTableWidgetItem(QString::number(s.catalogued)));
        m_unsorted->setItem(row, 2, new QTableWidgetItem(QString::number(s.remembered)));
    }
}

void LibraryFoldersDialog::markUnsorted()
{
    const QStringList roots = m_app.settings().watchedRoots();
    const QString folder = QDir::cleanPath(QFileDialog::getExistingDirectory(
        this, tr("Mark a Folder Unsorted"), roots.isEmpty() ? QDir::homePath() : roots.first()));
    if (folder.isEmpty() || folder == QStringLiteral("."))
        return;
    bool inLibrary = false;
    for (const QString &r : roots)
        inLibrary = inLibrary || folder == QDir::cleanPath(r) || folder.startsWith(QDir::cleanPath(r) + QLatin1Char('/'));
    if (!inLibrary) {
        QMessageBox::information(this, tr("Mark a Folder Unsorted"),
                                 tr("\u201c%1\u201d isn't in the library. Add it first.").arg(QDir::toNativeSeparators(folder)));
        return;
    }
    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int count = maint.captureCountUnderFolder(folder);
    if (QMessageBox::question(this, tr("Mark a Folder Unsorted"),
                              tr("Mark \u201c%1\u201d (and its subfolders) Unsorted?\n\nOf the %n photo(s) catalogued "
                                 "there, PhotoLife keeps one per run of shots and only remembers the others, "
                                 "dropping their catalogue entries. Photos you've named, rejected, starred, "
                                 "uploaded or marked are always kept. No file is changed, and Mark as Sorted "
                                 "brings everything back.",
                                 nullptr, count)
                                  .arg(QDir::toNativeSeparators(folder)))
        != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const scan::Unsorted::ThinResult r = scan::Unsorted::mark(m_app.database().connectionName(), folder);
    QApplication::restoreOverrideCursor();
    if (!r.ok) {
        QMessageBox::warning(this, tr("Mark a Folder Unsorted"), tr("Couldn't mark it: %1").arg(r.error));
        return;
    }
    m_dirty = true;
    reload();
    QMessageBox::information(this, tr("Mark a Folder Unsorted"),
                             tr("%1 photos in %2 runs: %3 kept in the catalogue, %4 only remembered.")
                                 .arg(r.photos)
                                 .arg(r.runs)
                                 .arg(r.kept)
                                 .arg(r.removed));
}

void LibraryFoldersDialog::markSorted()
{
    const auto rows = m_unsorted->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;
    const QString folder = m_unsorted->item(rows.first().row(), 0)->data(Qt::UserRole).toString();
    if (!scan::Unsorted::unmark(m_app.database().connectionName(), folder)) {
        QMessageBox::warning(this, tr("Mark as Sorted"), tr("Couldn't change it."));
        return;
    }
    m_dirty = true;
    reload();
    emit folderSorted(folder);
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
