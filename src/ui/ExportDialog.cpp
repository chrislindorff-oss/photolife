#include "ui/ExportDialog.h"

#include "collection/CollectionManifest.h"
#include "db/Database.h"
#include "util/PathSanitize.h"

#include "model/CaptureListModel.h"
#include "thumb/ThumbnailCache.h"
#include "ui/CaptureDelegate.h"
#include "ui/FileNamingDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListView>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

ExportDialog::ExportDialog(pl::Database &db, pl::thumb::ThumbnailCache &thumbs, int projectId,
                          const QString &treeName, const QList<qint64> &visibleTaxonIds,
                          bool treePhotographedOnly, const QString &initialDestFolder,
                          const QString &initialCollectionFolder, bool includeRaw,
                          bool thumbnailsOnly, QWidget *parent)
    : QDialog(parent), m_db(db), m_projectId(projectId), m_visibleTaxonIds(visibleTaxonIds),
      m_treePhotographedOnly(treePhotographedOnly), m_newDestText(initialDestFolder),
      m_collectionText(initialCollectionFolder)
{
    {
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.prepare(QStringLiteral("SELECT name FROM project WHERE id = ?"));
        q.addBindValue(projectId);
        m_projectName = q.exec() && q.next() ? q.value(0).toString() : treeName;
        m_treeFolderName = util::sanitizeFilenameComponent(m_projectName);
    }

    setWindowTitle(tr("Export Photo Collection"));
    setModal(true);
    resize(640, 640);

    auto *hint = new QLabel(
        tr("Copies matched photos in \"%1\" into a folder tree that mirrors its "
           "taxonomy, one folder per taxon, keeping the original filenames or renaming "
           "the copies to a pattern you choose. Your library photos are never moved, "
           "renamed or modified.")
            .arg(treeName),
        this);
    hint->setWordWrap(true);

    m_modeNew = new QRadioButton(tr("&Create a new collection"), this);
    m_modeUpdate = new QRadioButton(tr("&Update an existing collection (adds new photos and "
                                       "taxa; nothing already there is changed or removed)"),
                                    this);
    m_modeNew->setChecked(true);
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_modeNew);
    modeGroup->addButton(m_modeUpdate);
    connect(m_modeNew, &QRadioButton::clicked, this, &ExportDialog::onModeChanged);
    connect(m_modeUpdate, &QRadioButton::clicked, this, &ExportDialog::onModeChanged);

    m_destFolder = new QLineEdit(initialDestFolder, this);
    auto *browse = new QPushButton(tr("&Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &ExportDialog::browseForFolder);
    auto *destRow = new QHBoxLayout;
    destRow->addWidget(m_destFolder, 1);
    destRow->addWidget(browse);

    m_matchVisibleTree = new QCheckBox(
        tr("Match what's currently shown in the tree (respects the \"photographed "
           "only\", rank, and threatened-status filters)"),
        this);
    m_matchVisibleTree->setChecked(true);
    m_includeAuto = new QCheckBox(tr("Include auto-applied matches (not just confirmed)"), this);
    m_copyOriginals = new QRadioButton(tr("Original files"), this);
    m_copyThumbnails = new QRadioButton(
        tr("Thumbnails only (small %1 px JPEGs; date, camera and GPS kept)")
            .arg(thumb::ThumbnailCache::kGridPx),
        this);
    (thumbnailsOnly ? m_copyThumbnails : m_copyOriginals)->setChecked(true);
    auto *copyGroup = new QButtonGroup(this);
    copyGroup->addButton(m_copyOriginals);
    copyGroup->addButton(m_copyThumbnails);
    auto *copyRow = new QHBoxLayout;
    copyRow->addWidget(new QLabel(tr("Copy:"), this));
    copyRow->addWidget(m_copyOriginals);
    copyRow->addWidget(m_copyThumbnails);
    copyRow->addStretch(1);

    m_includeRaw = new QCheckBox(tr("Include RAW files (they can be very large)"), this);
    m_includeRaw->setChecked(includeRaw);

    // A pure review tool: lets you inspect just the auto-applied matches at
    // any time, independent of whether they're currently included in the
    // export -- only narrows the preview grid below, never the export itself.
    m_previewAll = new QRadioButton(tr("All included photos"), this);
    m_previewAll->setChecked(true);
    m_previewAutoOnly = new QRadioButton(tr("Auto-applied only"), this);
    auto *previewModeGroup = new QButtonGroup(this);
    previewModeGroup->setExclusive(true);
    previewModeGroup->addButton(m_previewAll);
    previewModeGroup->addButton(m_previewAutoOnly);
    auto *previewModeRow = new QHBoxLayout;
    previewModeRow->setContentsMargins(20, 0, 0, 0);
    previewModeRow->addWidget(new QLabel(tr("Preview:"), this));
    previewModeRow->addWidget(m_previewAll);
    previewModeRow->addWidget(m_previewAutoOnly);
    previewModeRow->addStretch(1);

    m_scaffoldEmpty = new QCheckBox(this);   // text set by updateScaffoldState()
    m_scaffoldEmpty->setChecked(true);
    m_includeCommonName = new QCheckBox(
        tr("Include common name in folder names (e.g. \"Anura  ·  Frogs and Toads\")"), this);

    m_renameFiles = new QCheckBox(tr("&Rename the copied files using a naming pattern"), this);
    m_configureNaming = new QPushButton(tr("&Naming Pattern…"), this);
    m_namingSummary = new QLabel(this);
    m_namingSummary->setWordWrap(true);
    m_namingSummary->setContentsMargins(20, 0, 0, 0);
    connect(m_renameFiles, &QCheckBox::toggled, this, &ExportDialog::onRenameToggled);
    connect(m_configureNaming, &QPushButton::clicked, this, &ExportDialog::configureNaming);
    auto *namingRow = new QHBoxLayout;
    namingRow->addWidget(m_renameFiles);
    namingRow->addWidget(m_configureNaming);
    namingRow->addStretch(1);

    m_folderStatus = new QLabel(this);
    m_folderStatus->setWordWrap(true);
    m_useSubfolder = new QPushButton(this);
    connect(m_useSubfolder, &QPushButton::clicked, this, [this] {
        m_destFolder->setText(QDir(m_destFolder->text().trimmed()).filePath(m_treeFolderName));
    });
    m_confirmFolder = new QCheckBox(
        tr("I'm sure this is the collection for \"%1\"").arg(m_projectName), this);
    connect(m_confirmFolder, &QCheckBox::toggled, this, &ExportDialog::updateOkState);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_folderStatus, 1);
    statusRow->addWidget(m_useSubfolder, 0, Qt::AlignTop);

    m_destLabel = new QLabel(this);
    m_destLabel->setBuddy(m_destFolder);
    auto *form = new QFormLayout;
    form->addRow(m_destLabel, destRow);
    form->addRow(QString(), statusRow);
    form->addRow(QString(), m_confirmFolder);

    m_previewModel = new model::CaptureListModel(db, thumbs, this);
    m_previewGrid = new QListView(this);
    m_previewGrid->setModel(m_previewModel);
    m_previewGrid->setItemDelegate(new CaptureDelegate(m_previewGrid));
    m_previewGrid->setViewMode(QListView::IconMode);
    m_previewGrid->setResizeMode(QListView::Adjust);
    m_previewGrid->setMovement(QListView::Static);
    m_previewGrid->setUniformItemSizes(true);
    m_previewGrid->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_previewGrid->setIconSize(QSize(96, 96));
    m_previewGrid->setGridSize(QSize(116, 136));
    m_previewGrid->setWordWrap(true);
    m_previewGrid->setSpacing(4);

    m_previewCount = new QLabel(this);
    m_previewCount->setWordWrap(true);

    connect(m_previewModel, &QAbstractItemModel::modelReset, this,
            &ExportDialog::updatePreviewCount);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(m_modeNew);
    layout->addWidget(m_modeUpdate);
    layout->addLayout(form);
    layout->addWidget(m_matchVisibleTree);
    layout->addWidget(m_includeAuto);
    layout->addLayout(previewModeRow);
    layout->addLayout(copyRow);
    layout->addWidget(m_includeRaw);
    layout->addWidget(m_scaffoldEmpty);
    layout->addWidget(m_includeCommonName);
    layout->addLayout(namingRow);
    layout->addWidget(m_namingSummary);
    layout->addWidget(m_previewCount);
    layout->addWidget(m_previewGrid, 1);
    layout->addWidget(m_buttons);

    connect(m_destFolder, &QLineEdit::textChanged, this, [this] {
        m_confirmFolder->setChecked(false);   // a confirmation is for one folder only
        updateFolderCheck();
        updateNamingState();
        updateOkState();
    });
    connect(m_matchVisibleTree, &QCheckBox::toggled, this, [this] {
        updatePreviewScope();
        updateOkState();
        updateScaffoldState();
    });
    connect(m_includeAuto, &QCheckBox::toggled, this, [this] {
        updatePreviewScope();
        updateScaffoldState();
    });
    connect(m_includeRaw, &QCheckBox::toggled, this, &ExportDialog::updatePreviewCount);
    connect(m_copyThumbnails, &QRadioButton::toggled, this, [this] {
        updateCopyModeState();
        updatePreviewCount();
    });
    // clicked (not toggled): an exclusive group fires toggled on both the
    // newly-checked and newly-unchecked button per switch, which would
    // reload the preview twice for one click.
    connect(m_previewAll, &QRadioButton::clicked, this, &ExportDialog::updatePreviewScope);
    connect(m_previewAutoOnly, &QRadioButton::clicked, this, &ExportDialog::updatePreviewScope);

    updatePreviewScope();
    updateScaffoldState();
    updateCopyModeState();
    onModeChanged();
}

void ExportDialog::updateCopyModeState()
{
    const bool thumbnails = m_copyThumbnails->isChecked();
    m_includeRaw->setEnabled(!thumbnails);
    m_includeRaw->setToolTip(
        thumbnails ? tr("Each photo's thumbnail is made from its JPEG, or from its RAW file "
                        "when it has no JPEG.")
                   : tr("When unticked, only JPEGs and other non-RAW files are copied. Photos "
                        "that have only a RAW file are left out."));
}

void ExportDialog::onModeChanged()
{
    const bool update = m_modeUpdate->isChecked();
    // Each mode remembers its own folder: a parent folder for a new
    // collection, the collection itself for an update.
    if (update != m_showingUpdate) {
        (m_showingUpdate ? m_collectionText : m_newDestText) = m_destFolder->text();
        m_showingUpdate = update;
        m_destFolder->setText(update ? m_collectionText : m_newDestText);
    }

    m_destLabel->setText(update ? tr("C&ollection folder:") : tr("&Destination:"));
    m_destFolder->setPlaceholderText(update
                                         ? tr("Choose the folder holding the existing collection…")
                                         : tr("Choose a destination folder…"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(update ? tr("Update Collection")
                                                            : tr("Export"));
    m_includeCommonName->setEnabled(!update);
    if (!update)
        m_includeCommonName->setToolTip(QString());
    m_copyOriginals->setEnabled(!update);
    m_copyThumbnails->setEnabled(!update);
    if (!update) {
        m_copyOriginals->setToolTip(QString());
        m_copyThumbnails->setToolTip(QString());
    }

    updateFolderCheck();
    updateNamingState();
    updateOkState();
    updatePreviewCount();
}

void ExportDialog::setFileNaming(bool rename, const collection::FileNamePattern &pattern,
                                 const QString &authorName)
{
    m_userPattern = pattern;
    m_userAuthor = authorName;
    m_userRename = rename && !pattern.isEmpty();
    updateNamingState();
}

void ExportDialog::onRenameToggled(bool rename)
{
    if (m_modeUpdate->isChecked())
        return;   // fixed to the collection's own choice
    // Ticking it with no pattern yet goes straight to building one; backing
    // out of that leaves the files' own names.
    m_userRename = rename && (!m_userPattern.isEmpty() || configureNaming());
    updateNamingState();
}

bool ExportDialog::configureNaming()
{
    FileNamingDialog dialog(m_db, m_projectId, m_userPattern, m_userAuthor, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    m_userPattern = dialog.pattern();
    m_userAuthor = dialog.authorName();
    m_userRename = true;
    updateNamingState();
    return true;
}

void ExportDialog::updateNamingState()
{
    const bool update = m_modeUpdate->isChecked();
    const collection::FileNamePattern &pattern = update ? m_collectionPattern : m_userPattern;
    const bool rename = update ? !m_collectionPattern.isEmpty() : m_userRename;
    {
        const QSignalBlocker blocker(m_renameFiles);
        m_renameFiles->setChecked(rename);
    }
    m_renameFiles->setEnabled(!update);
    m_configureNaming->setEnabled(!update);
    const QString tip = update ? tr("Fixed to match how this collection was first exported — "
                                    "changing it would add a second copy of every photo "
                                    "under its new name.")
                               : QString();
    m_renameFiles->setToolTip(tip);
    m_configureNaming->setToolTip(tip);
    m_namingSummary->setText(rename ? tr("Files are named: %1").arg(pattern.describe())
                                    : tr("Files keep their original names."));
}

void ExportDialog::updateFolderCheck()
{
    const QString path = m_destFolder->text().trimmed();
    m_collectionPattern = {};
    m_collectionAuthor.clear();
    m_useSubfolder->hide();
    m_confirmFolder->hide();
    if (!m_modeUpdate->isChecked()) {
        m_folderCheck = FolderCheck::Ok;
        if (path.isEmpty()) {
            m_folderStatus->clear();
        } else if (QDir(path).exists(m_treeFolderName)) {
            m_folderStatus->setText(
                tr("This folder already contains \"%1\" — new photos will be added to it, "
                   "like an update.")
                    .arg(m_treeFolderName));
        } else {
            m_folderStatus->setText(
                tr("A folder named \"%1\" will be created inside it.").arg(m_treeFolderName));
        }
        return;
    }

    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        m_folderCheck = FolderCheck::Blocked;
        m_folderStatus->setText(path.isEmpty()
                                    ? tr("Choose the folder that holds the existing collection "
                                         "(the one named after the tree, not its parent).")
                                    : tr("✗ This folder doesn't exist."));
        return;
    }

    // Folder-naming style: the collection's own, never the checkbox's current
    // state -- changing it would give every taxon a second, differently-named folder.
    // Likewise originals vs thumbnails: collections from before the marker
    // recorded it were always originals.
    bool commonNames = false;
    bool thumbnails = false;
    const auto manifest = collection::CollectionManifest::read(path);
    if (manifest) {
        commonNames = manifest->includeCommonName;
        thumbnails = manifest->thumbnailPx > 0;
        m_collectionPattern = manifest->fileNamePattern;
        m_collectionAuthor = manifest->authorName;
        if (manifest->projectId == m_projectId) {
            m_folderCheck = FolderCheck::Ok;
            m_folderStatus->setText(
                tr("✓ Recognised: the collection for \"%1\", last updated %2.")
                    .arg(m_projectName,
                         QLocale().toString(manifest->updatedAt.toLocalTime(), QLocale::ShortFormat)));
        } else {
            m_folderCheck = FolderCheck::Blocked;
            m_folderStatus->setText(tr("✗ This is the collection for \"%1\", not \"%2\".")
                                        .arg(manifest->projectName, m_projectName));
        }
    } else {
        // Collections exported before the marker file existed: recognise by
        // name, and infer the naming style from the folders already there.
        const QDir dir(path);
        for (const QString &child : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QStringList names = {child};
            names += QDir(dir.filePath(child)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            if (std::any_of(names.cbegin(), names.cend(), [](const QString &n) {
                    return n.contains(QStringLiteral("  ·  "));
                })) {
                commonNames = true;
                break;
            }
        }

        if (QFileInfo(path).fileName() == m_treeFolderName) {
            m_folderCheck = FolderCheck::Ok;
            m_folderStatus->setText(tr("✓ Folder name matches \"%1\".").arg(m_projectName));
        } else if (dir.exists(m_treeFolderName)) {
            m_folderCheck = FolderCheck::NeedsConfirm;
            m_folderStatus->setText(
                tr("⚠ This looks like the folder that <i>contains</i> the collection. "
                   "Updating it here would put new taxon folders beside \"%1\", not inside it.")
                    .arg(m_treeFolderName));
            m_useSubfolder->setText(tr("Use \"%1\"").arg(m_treeFolderName));
            m_useSubfolder->show();
            m_confirmFolder->show();
        } else {
            m_folderCheck = FolderCheck::NeedsConfirm;
            m_folderStatus->setText(
                tr("⚠ This folder's name doesn't match \"%1\", and it wasn't marked as a "
                   "PhotoLife collection. Check it's the right folder.")
                    .arg(m_projectName));
            m_confirmFolder->show();
        }
    }

    m_includeCommonName->setChecked(commonNames);
    m_includeCommonName->setToolTip(
        tr("Fixed to match how this collection was first exported — changing it would "
           "create a second, differently-named folder for every taxon."));

    (thumbnails ? m_copyThumbnails : m_copyOriginals)->setChecked(true);
    const QString copyTip = tr("Fixed to match how this collection was first exported — "
                               "changing it would add a second copy of every photo.");
    m_copyOriginals->setToolTip(copyTip);
    m_copyThumbnails->setToolTip(copyTip);
}

void ExportDialog::browseForFolder()
{
    const QString chosen = QFileDialog::getExistingDirectory(
        this,
        m_modeUpdate->isChecked() ? tr("Choose the Collection Folder")
                                  : tr("Choose a Destination Folder"),
        m_destFolder->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!chosen.isEmpty())
        m_destFolder->setText(chosen);
}

void ExportDialog::updateOkState()
{
    const bool nothingVisible = m_matchVisibleTree->isChecked() && m_visibleTaxonIds.isEmpty();
    const bool folderOk = m_folderCheck == FolderCheck::Ok
                          || (m_folderCheck == FolderCheck::NeedsConfirm
                              && m_confirmFolder->isChecked());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(
        !m_destFolder->text().trimmed().isEmpty() && !nothingVisible && folderOk);
}

void ExportDialog::updatePreviewScope()
{
    const bool nothingVisible = m_matchVisibleTree->isChecked() && m_visibleTaxonIds.isEmpty();
    m_previewGrid->setVisible(!nothingVisible);
    if (nothingVisible) {
        m_previewCount->setText(
            tr("Nothing is currently visible in the tree — uncheck \"Match what's currently "
               "shown\" above, or adjust the tree's filters, to export anything."));
        return;
    }

    if (m_matchVisibleTree->isChecked())
        m_previewModel->setTaxonSetScope(m_projectId, m_visibleTaxonIds);
    else
        m_previewModel->setProjectScope(m_projectId);

    QString statusFilter;
    if (m_previewAutoOnly->isChecked())
        statusFilter = QStringLiteral("auto");
    else if (m_includeAuto->isChecked())
        statusFilter = QStringLiteral("confirmedOrAuto");
    else
        statusFilter = QStringLiteral("confirmed");
    m_previewModel->setStatusFilter(statusFilter);
}

void ExportDialog::updatePreviewCount()
{
    // The preview shows a photo's JPEG when it has one, so a RAW preview
    // means the photo has nothing but a RAW file.
    int rawOnly = 0;
    if (!m_includeRaw->isChecked() && !m_copyThumbnails->isChecked()) {
        for (int row = 0; row < m_previewModel->rowCount(); ++row) {
            if (m_previewModel->index(row).data(model::CaptureListModel::PreviewIsRawRole).toBool())
                ++rawOnly;
        }
    }
    const QString rawNote =
        rawOnly > 0 ? QLatin1Char(' ')
                          + tr("%n of them only have a RAW file and will be skipped.", nullptr,
                               rawOnly)
                    : QString();

    m_previewCount->setText(
        m_previewAutoOnly->isChecked()
            ? tr("Showing %n auto-applied photo(s) for review.", nullptr,
                m_previewModel->rowCount())
            : m_modeUpdate->isChecked()
                ? tr("%n photo(s) in scope — any already in the collection are skipped.",
                     nullptr, m_previewModel->rowCount())
                : tr("%n photo(s) will be exported.", nullptr, m_previewModel->rowCount()));
    if (!m_previewAutoOnly->isChecked())
        m_previewCount->setText(m_previewCount->text() + rawNote);
}

void ExportDialog::updateScaffoldState()
{
    // The tree's "photographed only" filter counts auto-applied matches as
    // photographed, so when the export mirrors that view every taxon shown
    // either has photos to copy or is an ancestor of one (whose folder gets
    // created along the way regardless) -- unless auto-applied matches are
    // left out, in which case auto-only taxa would still end up empty.
    const bool mirrorsPhotographed = m_matchVisibleTree->isChecked() && m_treePhotographedOnly;
    const bool redundant = mirrorsPhotographed && m_includeAuto->isChecked();

    m_scaffoldEmpty->setEnabled(!redundant);
    m_scaffoldEmpty->setText(mirrorsPhotographed
                                 ? tr("Create folders for taxa with no exported photos")
                                 : tr("Create folders for taxa with no photos yet"));
    m_scaffoldEmpty->setToolTip(
        redundant ? tr("Every taxon shown in the tree has photos, so there are no empty "
                       "folders to create.")
        : mirrorsPhotographed
            ? tr("Taxa whose only matches are auto-applied are shown in the tree but "
                 "won't have any photos exported unless auto-applied matches are included.")
            : QString());
}

bool ExportDialog::updatesExistingCollection() const
{
    return m_modeUpdate->isChecked();
}

QString ExportDialog::destRoot() const
{
    return m_destFolder->text().trimmed();
}

collection::ExportOptions ExportDialog::options() const
{
    collection::ExportOptions options;
    options.includeAutoApplied = m_includeAuto->isChecked();
    options.includeRaw = m_includeRaw->isChecked();
    if (m_copyThumbnails->isChecked())
        options.thumbnailPx = thumb::ThumbnailCache::kGridPx;
    options.scaffoldEmptyFolders = m_scaffoldEmpty->isChecked();
    options.includeCommonName = m_includeCommonName->isChecked();
    if (updatesExistingCollection()) {
        options.fileNamePattern = m_collectionPattern;
        options.authorName = m_collectionAuthor;
    } else if (m_userRename) {
        options.fileNamePattern = m_userPattern;
        options.authorName = m_userAuthor;
    }
    options.updateExisting = updatesExistingCollection();
    options.reportUnexpectedFiles = updatesExistingCollection();
    if (m_matchVisibleTree->isChecked())
        options.restrictToTaxonIds = QSet<qint64>(m_visibleTaxonIds.begin(), m_visibleTaxonIds.end());
    return options;
}

} // namespace pl
