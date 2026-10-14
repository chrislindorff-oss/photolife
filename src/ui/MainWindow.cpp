#include "ui/MainWindow.h"

#include "app/Application.h"
#include "app/StorageStats.h"
#include "catalogue/BestShotStore.h"
#include "catalogue/UntreedMatchStore.h"
#include "db/Database.h"
#include "db/PostgresConnectionMonitor.h"
#include "model/CaptureListModel.h"
#include "model/ReferencePhotoModel.h"
#include "model/TaxonomyTreeModel.h"
#include "pl/Version.h"
#include "checklist/ChecklistImporter.h"
#include "checklist/ChecklistParser.h"
#include "collection/ExportService.h"
#include "coverage/CoverageCalculator.h"
#include "geo/LocalityFetcher.h"
#include "inat/ExifWriter.h"
#include "inat/InatImportService.h"
#include "inat/InatPhotoDownloader.h"
#include "lightroom/LightroomImporter.h"
#include "match/MatchReviewer.h"
#include "match/MatchService.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "net/TileCache.h"
#include "net/UpdateChecker.h"
#include "scan/CatalogueMaintenance.h"
#include "scan/Exif.h"
#include "scan/LibraryWatcher.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "sync/SyncActivityLog.h"
#include "taxonomy/InfraspecificFiller.h"
#include "taxonomy/OriginFetcher.h"
#include "taxonomy/AlaStatusSync.h"
#include "taxonomy/ApcOriginSync.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/ReferencePhotoFetcher.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "treefile/TreeFile.h"
#include "treefile/TreeImporter.h"
#include "util/PathSanitize.h"
#include "ui/AliasEditorDialog.h"
#include "ui/CatalogueConnectDialog.h"
#include "ui/CatalogueSettingsDialog.h"
#include "ui/CaptureDelegate.h"
#include "ui/PhotosOnComputerDialog.h"
#include "ui/CoveragePanel.h"
#include "ui/ExportDialog.h"
#include "ui/HelpWindow.h"
#include "ui/ImageViewer.h"
#include "ui/InatCoordinateConfirmDialog.h"
#include "ui/InatComparePage.h"
#include "ui/InatDownloadPage.h"
#include "ui/InatToolsView.h"
#include "ui/LibraryFoldersDialog.h"
#include "ui/ManageCataloguesDialog.h"
#include "ui/MapView.h"
#include "ui/NewProjectDialog.h"
#include "ui/PlaceConfirmDialog.h"
#include "ui/ReferencePhotoDialog.h"
#include "ui/ReviewPane.h"
#include "ui/ServerDialogs.h"
#include "ui/StaleMatchDialog.h"
#include "ui/TaxonConfirmDialog.h"
#include "ui/TaxonPickerDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListView>
#include <QListWidget>
#include <QTreeWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QProcess>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>
#include <QtConcurrent>

#include <cmath>
#include <functional>
#include <utility>

#include "db/Database.h"

namespace pl {
namespace {

// The modal progress window beginTaskProgress() shows. Unlike a plain
// QProgressDialog, it can't be dismissed while its task runs: Escape would
// otherwise just hide it (QDialog::reject) -- unlocking the rest of the app
// with the task still going -- so Escape and the title-bar close button act
// as Cancel instead (when the task can be cancelled) and the window stays up
// until endTaskProgress() takes it down.
class TaskProgressDialog : public QProgressDialog
{
public:
    TaskProgressDialog(const QString &label, bool cancellable, QWidget *parent)
        : QProgressDialog(label, cancellable ? QObject::tr("Cancel") : QString(), 0, 0, parent),
          m_cancellable(cancellable)
    {
        setWindowFlag(Qt::WindowCloseButtonHint, false);
    }

    void reject() override
    {
        if (m_cancellable)
            emit canceled();
    }

    // Cancel has been asked for; there's nothing more to cancel.
    void markCancelling()
    {
        m_cancellable = false;
        setCancelButtonText(QString());   // a null text removes the button
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
        if (m_cancellable)
            emit canceled();
    }

private:
    bool m_cancellable;
};

} // namespace

MainWindow::MainWindow(Application &app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    setWindowTitle(QString::fromLatin1(kAppName));

    m_model = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_treeModel = new model::TaxonomyTreeModel(m_app.database(), this);
    m_builder = new taxonomy::ProjectBuilder(m_app.inat(), m_app.taxonomyStore(), this);
    m_infraFiller = new taxonomy::InfraspecificFiller(m_app.inat(), m_app.taxonomyStore(), this);
    m_refPhotoFetcher =
        new taxonomy::ReferencePhotoFetcher(m_app.inat(), m_app.taxonomyStore(), this);
    m_checklistImporter =
        new checklist::ChecklistImporter(m_app.inat(), m_app.taxonomyStore(), this);
    m_treeImporter = new treefile::TreeImporter(m_app.inat(), m_app.taxonomyStore(), this);
    m_originFetcher =
        new taxonomy::OriginFetcher(m_app.inatBackground(), m_app.taxonomyStore(), this);
    connect(m_originFetcher, &taxonomy::OriginFetcher::projectFinished, this,
            [this](int projectId, bool ok, int written) {
                if (ok && written > 0 && projectId == currentProjectId())
                    refreshCoverage();
            });
    // Both keep their downloads under one folder shared by every catalogue.
    const QString sourcesDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("sources"));
    m_alaSync = new taxonomy::AlaStatusSync(m_app.http(), m_app.taxonomyStore(), sourcesDir, this);
    connect(m_alaSync, &taxonomy::AlaStatusSync::progress, this,
            [this](int index, int count, const QString &listName) {
                m_sourceSync.alaIndex = index;
                m_sourceSync.alaCount = count;
                m_sourceSync.alaLine = tr("Atlas of Living Australia — list %1 of %2: %3")
                                           .arg(index + 1)
                                           .arg(count)
                                           .arg(listName);
                updateSourceSyncProgress();
            });
    connect(m_alaSync, &taxonomy::AlaStatusSync::finished, this,
            [this](bool ok, const QString &error, int lists, int matched) {
                if (ok) {
                    m_app.settings().setSourceSyncedAt(QStringLiteral("ala"),
                                                       QDateTime::currentDateTime());
                    refreshCoverage();
                } else if (error != QLatin1String("cancelled")) {
                    qWarning().noquote() << "ALA conservation lists:" << error;
                }
                m_sourceSync.alaDone = true;
                m_sourceSync.alaLine =
                    ok ? tr("Atlas of Living Australia — done: %1 lists, %2 taxa in your trees "
                            "listed.")
                             .arg(lists)
                             .arg(matched)
                    : error == QLatin1String("cancelled")
                        ? tr("Atlas of Living Australia — cancelled.")
                        : tr("Atlas of Living Australia — failed: %1").arg(error);
                sourceSyncPartFinished();
            });
    m_apcSync = new taxonomy::ApcOriginSync(
        m_app.taxonomyStore(), sourcesDir,
        QStringLiteral("%1/%2").arg(QString::fromLatin1(kAppName), QString::fromLatin1(kAppVersion))
            .toUtf8(),
        this);
    connect(m_apcSync, &taxonomy::ApcOriginSync::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                const double mb = 1024.0 * 1024.0;
                m_sourceSync.apcLine =
                    total > 0 ? tr("Australian Plant Census — downloading %1 of %2 MB")
                                    .arg(received / mb, 0, 'f', 0)
                                    .arg(total / mb, 0, 'f', 0)
                              : tr("Australian Plant Census — downloading, %1 MB so far")
                                    .arg(received / mb, 0, 'f', 0);
                // The download is most of the wait; reading and saving follow.
                m_sourceSync.apcFraction = total > 0 ? 0.85 * double(received) / total : 0.0;
                updateSourceSyncProgress();
            });
    connect(m_apcSync, &taxonomy::ApcOriginSync::progress, this,
            [this](const QString &phase, int percent) {
                m_sourceSync.apcLine = tr("Australian Plant Census — %1").arg(phase);
                m_sourceSync.apcFraction = std::max(m_sourceSync.apcFraction, percent / 100.0);
                updateSourceSyncProgress();
            });
    connect(m_apcSync, &taxonomy::ApcOriginSync::finished, this,
            [this](bool ok, const QString &error, int matched) {
                if (ok) {
                    m_app.settings().setSourceSyncedAt(QStringLiteral("apc"),
                                                       QDateTime::currentDateTime());
                    refreshCoverage();
                } else if (error != QLatin1String("cancelled")) {
                    qWarning().noquote() << "Australian Plant Census:" << error;
                }
                m_sourceSync.apcDone = true;
                m_sourceSync.apcLine =
                    ok ? tr("Australian Plant Census — done: %1 plant taxa in your trees "
                            "matched.")
                             .arg(matched)
                    : error == QLatin1String("cancelled")
                        ? tr("Australian Plant Census — cancelled.")
                        : tr("Australian Plant Census — failed: %1").arg(error);
                sourceSyncPartFinished();
            });
    m_localityFetcher =
        new geo::LocalityFetcher(m_app.geocoder(), m_app.database().connectionName(), this);
    m_inatImportService =
        new inat::InatImportService(m_app.inatPhotoDownloader(), QString(), this);
    m_inatImportService->setExifWriter(&inat::writeExif);
    connect(m_inatImportService, &inat::InatImportService::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(tr("Downloading photos — %1 / %2").arg(done).arg(total));
                if (m_inatPipelineActive && !m_inatPipelineCancelled)
                    updateTaskProgress(done < total
                                           ? tr("Downloading photo %1 of %2…").arg(done + 1).arg(total)
                                           : tr("Downloaded %1 photo(s).").arg(total),
                                       done, total);
            });
    connect(m_inatImportService, &inat::InatImportService::finished, this,
            &MainWindow::onInatImportFinished);

    buildMenus();
    buildCentralWidget();
    applyCaptionFields(m_app.settings().captureCaptionFields());
    buildReferenceTreeDock();
    restoreLayout();

    connect(m_builder, &taxonomy::ProjectBuilder::progress, this,
            [this](const QString &phase, int done, int total) {
                statusBar()->showMessage(total > 0
                                             ? tr("%1 — %2 / %3").arg(phase).arg(done).arg(total)
                                             : phase);
                updateTaskProgress(phase, done, total);
            });
    connect(m_builder, &taxonomy::ProjectBuilder::confirmMoreSpecies, this,
            [this](int fetched, int total) {
                const auto reply = QMessageBox::question(
                    this, tr("Large Reference Tree"),
                    tr("This reference tree has reached %L1 species"
                       "%2.\n\nDownloading more means many more requests to "
                       "iNaturalist. Fetch the next %L3 species?\n\n"
                       "Choose No to stop here — the tree built so far is kept and "
                       "you can extend it later with Refresh Reference Tree.")
                        .arg(fetched)
                        .arg(total > fetched ? tr(" of about %L1").arg(total) : QString())
                        .arg(5000),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (reply == QMessageBox::Yes)
                    m_builder->continueFetching();
                else
                    m_builder->stopFetching();
            });
    auto &matcher = m_app.matchService();
    connect(&matcher, &match::MatchService::started, this,
            [this] {
                m_matchAction->setEnabled(false);
                statusBar()->showMessage(
                    m_matchingDownload           ? tr("Matching the downloaded iNaturalist photos…")
                    : m_groupMatchName.isEmpty() ? tr("Matching…")
                                                 : tr("Matching against \"%1\"…").arg(m_groupMatchName));
            });
    connect(&matcher, &match::MatchService::progress, this, [this](int done, int total) {
        statusBar()->showMessage(tr("Matching %1 / %2").arg(done).arg(total));
        if (m_matchingDownload && m_inatPipelineActive && !m_inatPipelineCancelled)
            updateTaskProgress(tr("Matching the downloaded photos — %1 of %2…").arg(done).arg(total),
                               done, total);
        if (m_matchProgressActive && !m_matchCancelled)
            updateTaskProgress(
                m_groupMatchName.isEmpty()
                    ? tr("Matching photo %1 of %2…").arg(done).arg(total)
                    : tr("Matching against \"%1\" — photo %2 of %3…")
                          .arg(m_groupMatchName)
                          .arg(done)
                          .arg(total),
                done, total);
    });
    connect(&matcher, &match::MatchService::finished, this,
            [this](match::MatchEngine::Stats s) {
                endMatchProgress();   // before any Renamed Photos question
                m_matchAction->setEnabled(true);
                m_model->reload();
                m_bestShotModel->reload();
                updateEmptyState();
                refreshCoverage();
                m_reviewPane->reload();
                updateReviewTabText();
                updateBestShotsTabText();
                // Asked once this handler (and any download pipeline's
                // progress window) has finished.
                if (!s.staleDecisions.isEmpty()) {
                    QTimer::singleShot(0, this, [this, stale = s.staleDecisions] {
                        resolveStaleDecisions(stale);
                    });
                }
                const QString group = std::exchange(m_groupMatchName, QString());
                const bool download = std::exchange(m_matchingDownload, false);
                if (download) {
                    // The downloaded taxa are no longer NEW -- refresh the badges.
                    m_inatView->downloadPage()->recheckAgainstLibrary();
                    finishInatPipeline(
                        !s.ok()       ? tr("Matching the downloaded photos failed: %1").arg(s.error)
                        : s.cancelled ? tr("Matching cancelled — the downloaded photos are in the "
                                           "library; run Match Library to finish matching them.")
                                      : tr("Added and matched %1 photo(s) from iNaturalist — %2 "
                                           "automatic, %3 to review.")
                                            .arg(s.captures)
                                            .arg(s.autoApplied)
                                            .arg(s.pending));
                    return;
                }
                if (!s.ok()) {
                    statusBar()->showMessage(tr("Match failed: %1").arg(s.error), 10000);
                    return;
                }
                if (!group.isEmpty()) {
                    statusBar()->showMessage(
                        tr("Matched against \"%1\" — checked %2 unresolved photo(s): "
                           "%3 automatic, %4 to review, %5 not in this group")
                            .arg(group)
                            .arg(s.captures)
                            .arg(s.autoApplied)
                            .arg(s.pending)
                            .arg(s.outsideGroup),
                        10000);
                    return;
                }
                statusBar()->showMessage(
                    tr("Matched — %1 automatic, %2 to review, %3 unmatched")
                        .arg(s.autoApplied).arg(s.pending - s.unmatched).arg(s.unmatched),
                    8000);
            });

    connect(m_builder, &taxonomy::ProjectBuilder::finished, this,
            [this](bool ok, const QString &error, int projectId) {
                const bool wasNewTree = m_builderIsNewTree;
                m_builderIsNewTree = false;
                m_newTreeAction->setEnabled(true);
                m_refreshTreeAction->setEnabled(true);
                endTaskProgress();
                if (!ok) {
                    statusBar()->showMessage(tr("Reference tree build failed: %1").arg(error),
                                             10000);
                    return;
                }
                statusBar()->showMessage(tr("Reference tree ready."), 6000);
                fetchMissingPlaceDetails();
                m_originFetcher->enqueue({projectId});
                syncAustralianSources(false, true);   // match the new taxa
                reloadProjectList();
                const int idx = m_projectCombo->findData(projectId);
                if (idx >= 0)
                    m_projectCombo->setCurrentIndex(idx);
                refreshCoverage();
                if (wasNewTree)
                    fetchInfraspecificTaxa();
            });

    connect(m_infraFiller, &taxonomy::InfraspecificFiller::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Checking species for subspecies/varieties — %1 / %2").arg(done).arg(total));
                updateTaskProgress(tr("Checking species for subspecies/varieties…"), done, total);
            });
    connect(m_infraFiller, &taxonomy::InfraspecificFiller::finished, this,
            [this](bool ok, const QString &error, int added) {
                m_fetchInfraAction->setEnabled(true);
                endTaskProgress();
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching subspecies/varieties failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    added > 0 ? tr("Added %1 subspecies/varieties.").arg(added)
                             : tr("No new subspecies or varieties found."),
                    6000);
                if (added > 0) {
                    mutateTreePreservingState(
                        [this] { m_treeModel->setProject(currentProjectId()); });
                    refreshCoverage();
                }
            });

    connect(m_refPhotoFetcher, &taxonomy::ReferencePhotoFetcher::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Fetching reference photos — %1 / %2").arg(done).arg(total));
                updateTaskProgress(tr("Fetching reference photos…"), done, total);
            });
    connect(m_refPhotoFetcher, &taxonomy::ReferencePhotoFetcher::finished, this,
            [this](bool ok, const QString &error, int updated) {
                if (m_fetchRefPhotosAction)
                    m_fetchRefPhotosAction->setEnabled(true);
                endTaskProgress();
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching reference photos failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    updated > 0 ? tr("Added %n reference photo(s).", nullptr, updated)
                                : tr("No new reference photos found."),
                    6000);
                if (m_refPhotoModel)
                    m_refPhotoModel->reload();
            });

    connect(&m_app.exportService(), &collection::ExportService::progress, this,
            [this](int phase, int done, int total) {
                if (m_exportCancelling)
                    return;
                QString label;
                switch (collection::ExportEngine::Phase(phase)) {
                case collection::ExportEngine::Phase::PreparingFolders:
                    label = tr("Creating folder %1 of %2…").arg(done).arg(total);
                    break;
                case collection::ExportEngine::Phase::CopyingPhotos:
                    label = (m_exportThumbnails ? tr("Making thumbnail %1 of %2…")
                                                : tr("Copying photo %1 of %2…"))
                                .arg(done)
                                .arg(total);
                    break;
                case collection::ExportEngine::Phase::CheckingCollection:
                    label = tr("Checking the collection for other files (%1 so far)…").arg(done);
                    break;
                }
                statusBar()->showMessage(label);
                updateTaskProgress(label, done, total);
            });
    connect(&m_app.exportService(), &collection::ExportService::finished, this,
            [this](collection::ExportSummary s) {
                m_exportTreePhotosAction->setEnabled(true);
                m_exportCancelling = false;
                endTaskProgress();
                if (s.cancelled) {
                    statusBar()->showMessage(
                        tr("Export cancelled after copying %n photo(s).", nullptr,
                           s.photosCopied),
                        6000);
                    return;
                }
                if (!s.ok() || s.photosFailed > 0 || !s.failures.isEmpty()) {
                    showExportProblems(s);
                    if (!s.ok())
                        return;
                }
                statusBar()->showMessage(
                    tr("Exported %1 new photo(s) into %2 new folder(s) (%3 already there, %4 "
                       "renamed to avoid a clash, %5 failed).")
                        .arg(s.photosCopied)
                        .arg(s.foldersCreated)
                        .arg(s.photosSkippedExisting)
                        .arg(s.photosRenamedForCollision)
                        .arg(s.photosFailed),
                    10000);
                if (!s.unexpectedFiles.isEmpty())
                    showUnexpectedExportFiles(s);
            });

    connect(m_localityFetcher, &geo::LocalityFetcher::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Fetching photo localities — %1 / %2").arg(done).arg(total));
            });
    connect(m_localityFetcher, &geo::LocalityFetcher::finished, this,
            [this](bool ok, const QString &error, int updated) {
                if (m_fetchLocalitiesAction)
                    m_fetchLocalitiesAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching photo localities failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    updated > 0 ? tr("Looked up %n new location(s).", nullptr, updated)
                                : tr("No new photo locations to look up."),
                    6000);
                m_model->reload();
                if (m_taxonModel)
                    m_taxonModel->reload();
                if (m_bestShotModel)
                    m_bestShotModel->reload();
            });

    connect(m_checklistImporter, &checklist::ChecklistImporter::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(tr("Importing checklist %1 / %2").arg(done).arg(total));
                updateTaskProgress(tr("Importing checklist…"), done, total);
            });
    connect(m_checklistImporter, &checklist::ChecklistImporter::finished, this,
            [this](bool ok, const QString &error, int imported, int skipped, int unresolved) {
                m_importChecklistAction->setEnabled(true);
                endTaskProgress();
                if (!ok) {
                    statusBar()->showMessage(tr("Checklist import failed: %1").arg(error), 10000);
                    return;
                }
                const int outside = m_checklistImporter->statusesOutsideProject();
                statusBar()->showMessage(
                    tr("Checklist imported — %1 added, %2 skipped, %3 unresolved")
                            .arg(imported).arg(skipped).arg(unresolved)
                        + (outside > 0
                               ? tr(" (statuses recorded for %n skipped taxa already known "
                                    "from other trees)",
                                    nullptr, outside)
                               : QString()),
                    10000);
                m_treeModel->setProject(currentProjectId());
                m_treeView->expandToDepth(1);
                refreshCoverage();
            });

    connect(m_treeImporter, &treefile::TreeImporter::progress, this,
            [this](int done, int total) {
                updateTaskProgress(tr("Importing reference tree…"), done, total);
            });
    connect(m_treeImporter, &treefile::TreeImporter::finished, this,
            [this](bool ok, const QString &error, int projectId, int imported, int lookedUp,
                   int unresolved) {
                m_importTreeAction->setEnabled(true);
                endTaskProgress();
                if (!ok) {
                    statusBar()->showMessage(tr("Reference tree import failed: %1").arg(error),
                                             10000);
                    return;
                }
                QString message = tr("Reference tree imported — %n taxa", nullptr, imported);
                if (lookedUp > 0)
                    message += tr(", %n looked up on iNaturalist", nullptr, lookedUp);
                if (unresolved > 0)
                    message += tr(", %n not found", nullptr, unresolved);
                statusBar()->showMessage(message + QLatin1Char('.'), 10000);
                // As after a tree build: place ancestry, origins, Australian
                // statuses for the new taxa, then show it.
                fetchMissingPlaceDetails();
                m_originFetcher->enqueue({projectId});
                syncAustralianSources(false, true);
                reloadProjectList();
                const int idx = m_projectCombo->findData(projectId);
                if (idx >= 0)
                    m_projectCombo->setCurrentIndex(idx);
                refreshCoverage();
            });

    auto &lightroomImporter = m_app.lightroomImporter();
    connect(&lightroomImporter, &lightroom::LightroomImporter::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Importing Lightroom keywords %1 / %2").arg(done).arg(total));
                updateTaskProgress(tr("Importing Lightroom Catalog…"), done, total);
            });
    connect(&lightroomImporter, &lightroom::LightroomImporter::finished, this,
            [this](lightroom::LightroomImportEngine::Stats stats) {
                m_importLightroomAction->setEnabled(true);
                endTaskProgress();
                if (!stats.ok()) {
                    statusBar()->showMessage(tr("Lightroom import failed: %1").arg(stats.error),
                                             10000);
                    return;
                }
                statusBar()->showMessage(
                    tr("Lightroom catalog imported — %1 photo(s) matched, %2 keyword(s) resolved")
                        .arg(stats.photosMatched)
                        .arg(stats.keywordsResolved),
                    8000);
                startMatch();
            });

    auto &scanner = m_app.scanService();
    connect(&scanner, &scan::ScanService::started, this, [this] { setScanUiRunning(true); });
    connect(&scanner, &scan::ScanService::progress, this, &MainWindow::onScanProgress);
    connect(&scanner, &scan::ScanService::finished, this, &MainWindow::onScanFinished);

    connect(&m_app.libraryWatcher(), &scan::LibraryWatcher::changeDetected, this, [this] {
        // A change mid-scan may land after the running scan passed that
        // folder, so queue a follow-up rather than ignoring it.
        if (!m_app.scanService().isRunning())
            statusBar()->showMessage(tr("Library folders changed — rescanning…"), 4000);
        requestScan();
    });

    m_model->reload();
    updateEmptyState();
    reloadProjectList();
    fetchMissingPlaceDetails();
    // Introduced taxa per tree region, from iNaturalist -- quietly, in the
    // background; a checklist's origins fill in (and override) as imported.
    if (m_app.catalogueEditable())
        m_originFetcher->enqueue(m_app.taxonomyStore().projectsMissingOrigins());
    syncAustralianSources(false, false);

    // The first tree's refresh (queued by reloadProjectList()) runs now,
    // before the window is shown -- not on the first event-loop turn after
    // show(), where a slow refresh stalls Qt's restore of the saved window
    // layout and the main-window layout later crashes (null raise()).
    if (m_coverageRefreshQueued)
        refreshCoverageNow();
    buildCatalogueCorner();
    setupSync();
}

bool MainWindow::hasAustralianTree() const
{
    auto &store = m_app.taxonomyStore();
    std::optional<taxonomy::Place> australia = store.placeByInatId(6744);
    if (!australia) {
        australia = taxonomy::Place{};
        australia->inatId = 6744;
    }
    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    if (!q.exec(QStringLiteral("SELECT DISTINCT place_inat_id FROM project "
                               "WHERE place_inat_id IS NOT NULL")))
        return false;
    while (q.next()) {
        taxonomy::Place place;
        if (const auto cached = store.placeByInatId(q.value(0).toLongLong()))
            place = *cached;
        place.inatId = q.value(0).toLongLong();
        if (taxonomy::TaxonomyStore::placeWithin(place, *australia))
            return true;
    }
    return false;
}

void MainWindow::updateAustralianSourcesNow()
{
    if (!ensureCanEdit())
        return;
    if (m_sourceSync.active)
        return;
    m_sourceSync = {};
    m_sourceSync.active = true;
    m_sourceSync.alaLine = tr("Atlas of Living Australia — finding the conservation lists…");
    m_sourceSync.apcLine = tr("Australian Plant Census — starting…");
    beginTaskProgress(tr("Updating Threatened & Introduced Status"), [this] {
        m_alaSync->cancel();
        m_apcSync->cancel();
        showTaskCancelling(tr("Cancelling…"));
    });
    updateSourceSyncProgress();
    // Either may already be running in the background; the window then
    // just follows it to the end.
    syncAustralianSources(true, false);
}

void MainWindow::updateSourceSyncProgress()
{
    if (!m_sourceSync.active)
        return;
    const double ala = m_sourceSync.alaDone ? 1.0
                       : m_sourceSync.alaCount > 0
                           ? double(m_sourceSync.alaIndex) / m_sourceSync.alaCount
                           : 0.0;
    const double apc = m_sourceSync.apcDone ? 1.0 : m_sourceSync.apcFraction;
    updateTaskProgress(m_sourceSync.alaLine + QLatin1Char('\n') + m_sourceSync.apcLine,
                       int(std::lround((ala + apc) * 500.0)), 1000);
}

void MainWindow::sourceSyncPartFinished()
{
    if (!m_sourceSync.active)
        return;   // a quiet background refresh
    updateSourceSyncProgress();
    if (!m_sourceSync.alaDone || !m_sourceSync.apcDone)
        return;
    m_sourceSync.active = false;
    endTaskProgress();
    statusBar()->showMessage(m_sourceSync.alaLine + QStringLiteral("  ") + m_sourceSync.apcLine,
                             15000);
}

void MainWindow::syncAustralianSources(bool force, bool rematchOnly)
{
    if (!m_app.catalogueEditable())
        return;   // a read-only shared copy: its editor keeps these current
    // ALA's state/national conservation lists and the APC's per-state plant
    // origins only mean anything for trees in Australia -- and the APC file
    // is ~100 MB, so don't fetch either for anyone without one.
    if (!force && !hasAustralianTree())
        return;
    auto due = [this](const QString &source) {
        const QDateTime last = m_app.settings().sourceSyncedAt(source);
        return !last.isValid() || last.daysTo(QDateTime::currentDateTime()) > 30;
    };
    // Unless forced, both re-match from their kept downloads when those are
    // recent enough -- so a new tree, or a catalogue that hasn't synced
    // these yet, doesn't download them all again.
    if (force || rematchOnly || due(QStringLiteral("ala")))
        m_alaSync->start(force);
    if (force || rematchOnly || due(QStringLiteral("apc")))
        m_apcSync->start(force);
}

MainWindow::~MainWindow() = default;

void MainWindow::fetchMissingPlaceDetails()
{
    // Which regions lie within which decides where a region's conservation
    // statuses count (see computeCoverage), and a region's outline is drawn
    // on the map and bounds its locality filter. Trees built before those
    // were recorded, or from a place search that didn't return them, get
    // them here -- quietly, in the background; bounding boxes stand in until
    // they arrive.
    if (!m_app.catalogueEditable())
        return;   // a read-only shared copy: its editor fetches these
    auto &store = m_app.taxonomyStore();
    QList<qint64> ids = store.projectPlacesMissingAncestry();
    for (qint64 id : store.projectPlacesMissingGeometry()) {
        if (!ids.contains(id))
            ids.append(id);
    }
    for (qsizetype i = 0; i < ids.size(); i += 30) {
        m_app.inatBackground().fetchPlaces(ids.mid(i, 30), [this](net::Outcome<QList<taxonomy::Place>> out) {
            if (!out.ok())
                return;   // offline: try again next launch
            auto &store = m_app.taxonomyStore();
            bool changed = false;
            for (const taxonomy::Place &p : std::as_const(out.value)) {
                // Keeps whichever of the two was already known.
                changed = store.upsertPlace(p) || changed;
                if (p.ancestorIds.isEmpty())
                    store.markPlaceAncestryUnavailable(p.inatId);
                if (p.geometryGeoJson.isEmpty())
                    store.markPlaceGeometryUnavailable(p.inatId);
            }
            if (changed)
                refreshCoverage();
        });
    }
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QMenu *libraryMenu = menuBar()->addMenu(tr("&Library"));
    QMenu *treeMenu = menuBar()->addMenu(tr("&Reference Tree"));
    QMenu *matchingMenu = menuBar()->addMenu(tr("&Matching"));
    QMenu *inatMenu = menuBar()->addMenu(tr("i&Naturalist Tools"));
    m_viewMenu = menuBar()->addMenu(tr("&View"));
    m_serverMenu = menuBar()->addMenu(tr("&Server"));
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));

    buildFileMenu(fileMenu);
    buildLibraryMenu(libraryMenu);
    buildReferenceTreeMenu(treeMenu);
    buildViewMenu();                  // must precede buildMatchingMenu(): creates m_viewReviewAction
    buildMatchingMenu(matchingMenu);  // reuses m_matchAction + m_viewReviewAction
    buildInatMenu(inatMenu);          // after buildViewMenu(): switches via m_viewInatAction
    buildHelpMenu(helpMenu);
    buildServerMenu(m_serverMenu);
    buildToolBar();                   // last: reuses actions from every menu above + m_filterCombo

    // A local SQLite file doesn't have a meaningful "connected" state, so
    // this only appears for a shared Postgres catalogue -- and it checks
    // Database::backendFor() (what's actually open), not Settings, since
    // that's what the indicator needs to be honest about.
    if (Database::backendFor(m_app.database().connectionName())
        == CatalogueDescriptor::Backend::Postgres) {
        m_pgStatusLabel = new QLabel(this);
        m_pgStatusLabel->setContentsMargins(0, 0, 8, 0);
        menuBar()->setCornerWidget(m_pgStatusLabel, Qt::TopRightCorner);

        const CatalogueDescriptor descriptor = m_app.settings().catalogueDescriptor();
        const QString target = QStringLiteral("%1@%2:%3/%4")
                                    .arg(descriptor.pgUser, descriptor.pgHost)
                                    .arg(descriptor.pgPort)
                                    .arg(descriptor.pgDbName);

        m_pgMonitor = new PostgresConnectionMonitor(m_app.database().connectionName(), this);
        connect(m_pgMonitor, &PostgresConnectionMonitor::connectedChanged, this,
                [this, target](bool connected) {
                    m_pgStatusLabel->setText(
                        connected ? tr("● Postgres Connected")
                                 : tr("● Postgres Disconnected"));
                    m_pgStatusLabel->setStyleSheet(
                        QStringLiteral("color: %1;")
                            .arg(connected
                                     ? pl::themeColors(pl::currentThemeVariant()).positive.name()
                                     : pl::themeColors(pl::currentThemeVariant()).danger.name()));
                    m_pgStatusLabel->setToolTip(
                        connected ? tr("Connected to %1").arg(target)
                                 : tr("Lost connection to %1").arg(target));
                });
        m_pgMonitor->start();
    }
}

void MainWindow::buildFileMenu(QMenu *fileMenu)
{
    // Catalogues: the Local one and any Server ones -- completely separate.
    QMenu *switchMenu = fileMenu->addMenu(tr("Switch &Catalogue"));
    connect(switchMenu, &QMenu::aboutToShow, this, [this, switchMenu] {
        switchMenu->clear();
        const QString active = m_app.settings().activeProfile().id;
        for (const Settings::CatalogueProfile &p : m_app.settings().profiles()) {
            QAction *a = switchMenu->addAction(p.server ? tr("Server: %1").arg(p.name) : p.name);
            a->setCheckable(true);
            a->setChecked(p.id == active);
            connect(a, &QAction::triggered, this, [this, id = p.id] { switchToCatalogue(id); });
        }
    });
    fileMenu->addAction(tr("&Manage Catalogues…"), this, &MainWindow::showCatalogueSettings);


    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
}

void MainWindow::buildLibraryMenu(QMenu *libraryMenu)
{
    m_addFolderAction = libraryMenu->addAction(tr("&Add Folder to Library…"),
                                               this, &MainWindow::addWatchedFolder);
    m_addFolderAction->setShortcut(QKeySequence::Open);

    m_manageFoldersAction = libraryMenu->addAction(tr("&Manage Library Folders…"),
                                                   this, &MainWindow::manageLibraryFolders);

    m_importLightroomAction = libraryMenu->addAction(tr("Import &Lightroom Catalog…"),
                                                     this, &MainWindow::importLightroom);

    libraryMenu->addSeparator();
    m_scanAction = libraryMenu->addAction(tr("&Rescan Library"), this, &MainWindow::startScan);
    m_scanAction->setShortcut(QKeySequence::Refresh);

    m_cancelAction = libraryMenu->addAction(tr("&Stop Scan"),
                                            &m_app.scanService(), &scan::ScanService::cancel);
    m_cancelAction->setEnabled(false);

    libraryMenu->addSeparator();
    libraryMenu->addAction(tr("Fix RAW Photo &Locations…"), this, &MainWindow::fixRawGeolocation);
    m_fetchLocalitiesAction = libraryMenu->addAction(tr("Fetch Photo &Localities"),
                                                     this, &MainWindow::fetchPhotoLocalities);
    libraryMenu->addAction(tr("Remove Photos With &Missing Files…"),
                           this, &MainWindow::removeMissingCaptures);
}

void MainWindow::buildReferenceTreeMenu(QMenu *treeMenu)
{
    // m_projectCombo (in the Reference Trees dock) stays the persistent
    // selector for which tree/project is active; every action here operates
    // on whatever tree currentProjectId() currently returns.
    m_newTreeAction = treeMenu->addAction(tr("&New Reference Tree…"),
                                          this, &MainWindow::newReferenceTree);
    m_deleteTreeAction = treeMenu->addAction(tr("&Delete Reference Tree…"),
                                             this, &MainWindow::deleteReferenceTree);
    m_importTreeAction = treeMenu->addAction(tr("I&mport Reference Tree…"),
                                             this, &MainWindow::importReferenceTree);
    treeMenu->addAction(tr("E&xport Reference Tree…"), this, &MainWindow::exportReferenceTree);

    treeMenu->addSeparator();
    m_refreshTreeAction = treeMenu->addAction(tr("Re&fresh Reference Tree"),
                                              this, &MainWindow::refreshReferenceTree);
    m_addTaxonAction = treeMenu->addAction(tr("&Add Taxon to Reference Tree…"),
                                           this, &MainWindow::addTaxonToReferenceTree);
    m_fetchInfraAction = treeMenu->addAction(tr("Fetch &Subspecies/Varieties…"),
                                             this, &MainWindow::fetchInfraspecificTaxa);
    m_importChecklistAction = treeMenu->addAction(tr("&Import Checklist…"),
                                                  this, &MainWindow::importChecklist);

    treeMenu->addSeparator();
    m_fetchRefPhotosAction = treeMenu->addAction(tr("Fetch Reference &Photos"),
                                                 this, &MainWindow::fetchReferencePhotos);
    treeMenu->setToolTipsVisible(true);
    QAction *updateStatuses = treeMenu->addAction(
        tr("Update Threatened && Introduced Status (Australia)"), this,
        &MainWindow::updateAustralianSourcesNow);
    updateStatuses->setToolTip(
        tr("Re-downloads each state's and the national (EPBC) conservation lists from the "
           "Atlas of Living Australia, and native/introduced status by state for plants from "
           "the Australian Plant Census (about 100 MB). Both also refresh by themselves "
           "about monthly."));

    treeMenu->addSeparator();
    m_exportTreePhotosAction = treeMenu->addAction(tr("&Export Photo Collection…"),
                                                   this, &MainWindow::exportReferenceTreePhotos);
}

void MainWindow::buildViewMenu()
{
    m_viewMenu->setToolTipsVisible(true);   // so the Display submenu's tooltip shows

    m_viewModeGroup = new QActionGroup(this);
    m_viewModeGroup->setExclusive(true);

    m_viewTreeAction = m_viewMenu->addAction(tr("&Reference Tree"));
    m_viewTreeAction->setCheckable(true);
    m_viewTreeAction->setObjectName(QStringLiteral("viewTreeAction"));
    m_viewModeGroup->addAction(m_viewTreeAction);

    m_viewLibraryAction = m_viewMenu->addAction(tr("&All Library Photos"));
    m_viewLibraryAction->setCheckable(true);
    m_viewLibraryAction->setObjectName(QStringLiteral("viewLibraryAction"));
    m_viewModeGroup->addAction(m_viewLibraryAction);

    m_viewReviewAction = m_viewMenu->addAction(tr("&Review Unmatched"));
    m_viewReviewAction->setCheckable(true);
    m_viewReviewAction->setObjectName(QStringLiteral("viewReviewAction"));
    m_viewModeGroup->addAction(m_viewReviewAction);

    m_viewInatAction = m_viewMenu->addAction(tr("&iNaturalist"));
    m_viewInatAction->setCheckable(true);
    m_viewInatAction->setObjectName(QStringLiteral("viewInatAction"));
    m_viewModeGroup->addAction(m_viewInatAction);

    m_viewMenu->addSeparator();
    auto *displayMenu = m_viewMenu->addMenu(tr("&Display…"));
    displayMenu->menuAction()->setToolTip(
        tr("Choose what to show under each photo, in the Photos of Tree Selection "
           "and All Library Photos grids"));

    struct FieldOption { int bit; QString label; };
    const QList<FieldOption> fieldOptions = {
        {model::CaptureListModel::CaptionName, tr("Name")},
        {model::CaptureListModel::CaptionTaxon, tr("Matched Taxon")},
        {model::CaptureListModel::CaptionDate, tr("Date Taken")},
        {model::CaptureListModel::CaptionFilename, tr("Filename")},
        {model::CaptureListModel::CaptionLocality, tr("Locality")},
        {model::CaptureListModel::CaptionFileType, tr("File Type")},
    };
    const int initialFields = m_app.settings().captureCaptionFields();
    for (const FieldOption &opt : fieldOptions) {
        auto *box = new QCheckBox(opt.label, displayMenu);
        box->setChecked(initialFields & opt.bit);
        connect(box, &QCheckBox::toggled, this, [this, bit = opt.bit](bool on) {
            int fields = m_app.settings().captureCaptionFields();
            fields = on ? (fields | bit) : (fields & ~bit);
            applyCaptionFields(fields);
        });
        auto *action = new QWidgetAction(displayMenu);
        action->setDefaultWidget(box);
        displayMenu->addAction(action);
    }

    m_viewMenu->addSeparator();
    QAction *showLocality = m_viewMenu->addAction(tr("Show Tree &Locality on Map"));
    showLocality->setCheckable(true);
    showLocality->setChecked(m_app.settings().showMapLocality());
    showLocality->setToolTip(tr("Outline the active reference tree's region on the map."));
    connect(showLocality, &QAction::toggled, this, [this](bool on) {
        m_app.settings().setShowMapLocality(on);
        if (m_mapView)
            m_mapView->setShowLocality(on);
    });

    m_darkModeAction = m_viewMenu->addAction(tr("&Dark Mode"));
    m_darkModeAction->setCheckable(true);
    m_darkModeAction->setObjectName(QStringLiteral("darkModeAction"));
    m_darkModeAction->setChecked(m_app.settings().darkModeEnabled());
    connect(m_darkModeAction, &QAction::toggled, this, [this](bool on) {
        m_app.settings().setDarkModeEnabled(on);
        pl::applyTheme(*qApp, on ? pl::ThemeVariant::Dark : pl::ThemeVariant::Light);
    });
}

void MainWindow::buildMatchingMenu(QMenu *matchingMenu)
{
    m_matchAction = matchingMenu->addAction(tr("&Match Library"), this, &MainWindow::startMatch);

    matchingMenu->addSeparator();
    // Same QAction as View's radio group -- Qt keeps both menus' copies in
    // sync (checked state, QActionGroup exclusivity) since they're one object.
    matchingMenu->addAction(m_viewReviewAction);
    matchingMenu->addAction(tr("&Learned Names…"), this, [this] {
        if (!ensureCanEdit())
            return;
        AliasEditorDialog dialog(m_app.database(), this);
        connect(&dialog, &AliasEditorDialog::aliasesChanged, this, [this] {
            statusBar()->showMessage(
                tr("Forgotten names take effect on the next Match Library run."), 6000);
        });
        dialog.exec();
    });
}

void MainWindow::buildInatMenu(QMenu *inatMenu)
{
    // These just bring up the iNaturalist view -- m_inatView itself is only
    // built later, in buildCentralWidget(), hence the lambdas.
    inatMenu->addAction(tr("&Download My Observations"), this, [this] {
        m_viewInatAction->setChecked(true);
        m_inatView->showDownloadPage();
    });

    inatMenu->addAction(tr("&Compare Species with Trees"), this, [this] {
        m_viewInatAction->setChecked(true);
        m_inatView->showComparePage();
    });

    inatMenu->addSeparator();
    inatMenu->addAction(tr("iNaturalist &Account"), this, [this] {
        m_viewInatAction->setChecked(true);
        m_inatView->accountBar()->focusUsername();
    });
}

void MainWindow::buildHelpMenu(QMenu *helpMenu)
{
    QAction *helpContents = helpMenu->addAction(
        tr("%1 &Help").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showHelp);
    helpContents->setShortcut(QKeySequence::HelpContents);
    helpMenu->addSeparator();
    QAction *checkUpdate = helpMenu->addAction(tr("Check for &Updates…"), this, [this] {
        auto &checker = m_app.updateChecker();
        statusBar()->showMessage(tr("Checking for updates…"), 4000);
        checker.check();
    });
    checkUpdate->setMenuRole(QAction::ApplicationSpecificRole);
    helpMenu->addAction(tr("Storage &Usage…"), this, &MainWindow::showStorageUsage);
    QAction *about = helpMenu->addAction(
        tr("&About %1").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);

    connect(&m_app.updateChecker(), &net::UpdateChecker::upToDate, this, [this](const QString &v) {
        QMessageBox::information(this, tr("Check for Updates"),
                                tr("PhotoLife %1 is up to date.").arg(v));
    });
    connect(&m_app.updateChecker(), &net::UpdateChecker::updateAvailable, this,
            [this](const QString &latest, const QString &url) {
                QMessageBox box(QMessageBox::Information, tr("Update Available"),
                                tr("PhotoLife %1 is available.").arg(latest), QMessageBox::Close,
                                this);
                if (!url.isEmpty()) {
                    box.addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
                    if (box.exec() == QMessageBox::AcceptRole)
                        QDesktopServices::openUrl(QUrl(url));
                } else {
                    box.exec();
                }
            });
    connect(&m_app.updateChecker(), &net::UpdateChecker::checkFailed, this,
            [this](const QString &err) {
                statusBar()->showMessage(tr("Update check failed: %1").arg(err), 8000);
            });
}

void MainWindow::buildToolBar()
{
    QToolBar *toolbar = addToolBar(tr("Library"));
    toolbar->setObjectName(QStringLiteral("libraryToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(m_addFolderAction);
    toolbar->addAction(m_scanAction);
    toolbar->addAction(m_cancelAction);

    toolbar->addSeparator();
    toolbar->addAction(m_matchAction);

    toolbar->addSeparator();
    toolbar->addAction(m_viewTreeAction);
    toolbar->addAction(m_viewLibraryAction);
    toolbar->addAction(m_viewReviewAction);
    toolbar->addAction(m_viewInatAction);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("Show: "), toolbar));
    m_filterCombo = new QComboBox(toolbar);
    m_filterCombo->addItem(tr("All"), QString());
    m_filterCombo->addItem(tr("Auto-matched"), QStringLiteral("auto"));
    m_filterCombo->addItem(tr("Needs review"), QStringLiteral("pending"));
    m_filterCombo->addItem(tr("Unmatched"), QStringLiteral("unmatched"));
    m_filterCombo->addItem(tr("Confirmed"), QStringLiteral("confirmed"));
    connect(m_filterCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_model->setStatusFilter(m_filterCombo->itemData(i).toString());
        updateEmptyState();
    });
    toolbar->addWidget(m_filterCombo);
}

void MainWindow::buildReferenceTreeDock()
{
    auto *dock = new QDockWidget(tr("Reference Trees"), this);
    dock->setObjectName(QStringLiteral("referenceTreeDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto *panel = new QWidget(dock);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);

    m_projectCombo = new QComboBox(panel);
    m_projectCombo->setContextMenuPolicy(Qt::CustomContextMenu);
    m_projectCombo->setToolTip(tr("The reference tree shown. Right-click for Rename and more."));
    connect(m_projectCombo, &QWidget::customContextMenuRequested, this,
            &MainWindow::showTreeSelectorMenu);
    connect(m_projectCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_treeModel->setProject(index >= 0 ? m_projectCombo->itemData(index).toInt() : -1);
        m_treeView->expandToDepth(1);
        refreshCoverage();
        m_inatView->followActiveTree(currentProjectId());

        // Land the user on the newly chosen tree: select its top node, scroll
        // to the top, give the tree keyboard focus, and switch to the photo
        // grid that follows the selection.
        const QModelIndex root = m_treeModel->index(0, 0);
        if (root.isValid()) {
            m_treeView->setCurrentIndex(root);
            m_treeView->scrollToTop();
            m_treeView->setFocus();
        }
        switchToTreeTab(m_tabs->widget(0));   // Photos of Tree Selection
    });

    m_treeSearch = new QLineEdit(panel);
    m_treeSearch->setPlaceholderText(tr("Search taxa — jumps as you type, Enter for next"));
    m_treeSearch->setClearButtonEnabled(true);
    connect(m_treeSearch, &QLineEdit::textChanged, this, &MainWindow::onTreeSearchChanged);
    connect(m_treeSearch, &QLineEdit::returnPressed, this, &MainWindow::onTreeSearchNext);

    m_photographedOnly = new QCheckBox(tr("Only taxa I've photographed"), panel);
    connect(m_photographedOnly, &QCheckBox::toggled, this, [this](bool on) {
        const bool wasCollapsed = collectExpandedTaxa().isEmpty();
        mutateTreePreservingState([this, on] { m_treeModel->setPhotographedOnly(on); });
        if (on && wasCollapsed)
            m_treeView->expandToDepth(6);   // one-shot reveal of the photographed taxa
        onTreeSelectionChanged();
    });

    m_coverageFilterCombo = new QComboBox(panel);
    m_coverageFilterCombo->addItem(tr("All coverage"),
                                   int(model::TaxonomyTreeModel::CoverageFilter::All));
    m_coverageFilterCombo->addItem(tr("Fully covered"),
                                   int(model::TaxonomyTreeModel::CoverageFilter::Complete));
    m_coverageFilterCombo->addItem(tr("Not fully covered"),
                                   int(model::TaxonomyTreeModel::CoverageFilter::Incomplete));
    m_coverageFilterCombo->setToolTip(
        tr("Show only the taxa whose Coverage is complete (X / X, or ✔ for a species) or "
           "incomplete, among the ranks you've chosen to show. Greyed rows aren't a match "
           "themselves — they're just the path to one."));
    connect(m_coverageFilterCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        const auto filter = model::TaxonomyTreeModel::CoverageFilter(
            m_coverageFilterCombo->itemData(i).toInt());
        const bool wasCollapsed = collectExpandedTaxa().isEmpty();
        mutateTreePreservingState([this, filter] { m_treeModel->setCoverageFilter(filter); });
        if (filter != model::TaxonomyTreeModel::CoverageFilter::All && wasCollapsed)
            m_treeView->expandToDepth(6);   // one-shot reveal of the matches
        onTreeSelectionChanged();
    });

    m_rankFilterButton = new QToolButton(panel);
    m_rankFilterButton->setText(tr("Ranks…"));
    m_rankFilterButton->setToolTip(tr("Choose which taxonomic ranks to show in the tree"));
    m_rankFilterButton->setPopupMode(QToolButton::InstantPopup);
    m_rankFilterMenu = new QMenu(m_rankFilterButton);
    m_rankFilterButton->setMenu(m_rankFilterMenu);
    connect(m_treeModel, &QAbstractItemModel::modelReset, this, &MainWindow::rebuildRankFilterMenu);

    m_treeView = new QTreeView(panel);
    m_treeView->setModel(m_treeModel);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_treeView->setColumnWidth(0, 560);
    m_treeView->setAutoScroll(false);   // keep shallow names visible when selecting deep nodes
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onTreeSelectionChanged);
    connect(m_treeView, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &) { switchToTreeTab(m_tabs->widget(0)); });   // Photos of Tree Selection
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QWidget::customContextMenuRequested,
            this, &MainWindow::showTreeContextMenu);

    m_coveragePanel = new CoveragePanel(panel);
    connect(m_coveragePanel, &CoveragePanel::tierClicked, this,
            [this](const QString &status) { applyThreatenedFilter(false, status); });
    connect(m_coveragePanel, &CoveragePanel::anyThreatenedClicked, this,
            [this] { applyThreatenedFilter(true, QString()); });
    connect(m_coveragePanel, &CoveragePanel::introducedClicked, this,
            [this] { applyIntroducedFilter(!m_treeModel->introducedOnly()); });
    connect(m_coveragePanel, &CoveragePanel::clearFilterRequested, this, [this] {
        applyThreatenedFilter(false, QString());
        applyIntroducedFilter(false);
    });

    auto *split = new QSplitter(Qt::Vertical, panel);
    split->addWidget(m_treeView);
    split->addWidget(m_coveragePanel);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    layout->addWidget(m_projectCombo);
    layout->addWidget(m_treeSearch);
    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(m_photographedOnly);
    filterRow->addStretch(1);
    filterRow->addWidget(m_coverageFilterCombo);
    layout->addLayout(filterRow);
    layout->addWidget(m_rankFilterButton);
    layout->addWidget(split, 1);
    dock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    resizeDocks({dock}, {360}, Qt::Horizontal);

    // The dock's own close button hides it with no other way back short of
    // this menu entry -- and mainWindow/state persists that hidden state
    // across restarts (restoreLayout(), below), so without this there would
    // be no way to get the panel back at all once closed.
    QAction *toggle = dock->toggleViewAction();
    toggle->setText(tr("Reference Trees &Panel"));
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(toggle);
}

int MainWindow::currentProjectId() const
{
    const QVariant data = m_projectCombo->currentData();
    return data.isValid() ? data.toInt() : -1;
}

void MainWindow::refreshCoverage()
{
    if (m_coverageRefreshQueued)
        return;
    m_coverageRefreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        if (m_coverageRefreshQueued)   // not already run by refreshCoverageNow()
            refreshCoverageNow();
    });
}

void MainWindow::refreshCoverageNow()
{
    m_coverageRefreshQueued = false;
    // Phase timings, logged when the whole refresh is slow -- on a remote
    // catalogue they show which step the network time goes to.
    QElapsedTimer total, phase;
    total.start();
    phase.start();
    QStringList timings;
    auto lap = [&](const char *name) {
        timings << QStringLiteral("%1 %2").arg(QLatin1String(name)).arg(phase.restart());
    };
    auto logIfSlow = [&](int pid) {
        const qint64 ms = total.elapsed();
        if (ms >= 250)
            qInfo().noquote() << QStringLiteral("refreshCoverage(tree %1) %2 ms: %3")
                                     .arg(pid)
                                     .arg(ms)
                                     .arg(timings.join(QStringLiteral(", ")));
    };

    const int pid = currentProjectId();
    if (m_refPhotoModel)
        m_refPhotoModel->setProject(pid);
    lap("refPhotos");
    if (m_mapView)
        m_mapView->setActiveLocality(
            pid > 0 ? m_app.taxonomyStore().projectLocalityBox(pid) : std::nullopt,
            pid > 0 ? m_app.taxonomyStore().projectLocalityShape(pid) : std::nullopt);
    lap("locality");
    reloadUntreedTaxa();
    lap("untreed");
    if (pid <= 0 || !m_app.database().isOpen()) {
        m_coveragePanel->clear();
        mutateTreePreservingState([this] { m_treeModel->setCoverage({}); });
        onTreeSelectionChanged();
        lap("tree");
        logIfSlow(pid);
        return;
    }
    m_coverage = coverage::computeCoverage(m_app.database().connectionName(), pid);
    lap("coverage");
    // Stored only where the catalogue may change; a read-only shared copy
    // has the ones its editor stored.
    if (m_app.catalogueEditable())
        coverage::pickRepresentatives(m_app.database().connectionName(), pid);
    lap("representatives");
    m_coveragePanel->setCoverage(m_coverage);

    QStringList photoUrls;
    for (const auto &leaf : m_app.taxonomyStore().projectLeafPhotos(pid)) {
        if (!leaf.photoUrl.isEmpty())
            photoUrls << leaf.photoUrl;
    }
    const auto usage = m_app.photoCache().diskUsage(photoUrls);
    m_coveragePanel->setCacheUsage(usage.count, photoUrls.size(), usage.bytes);
    lap("leafPhotos");

    mutateTreePreservingState([this] { m_treeModel->setCoverage(m_coverage); });
    updateMissingList();
    onTreeSelectionChanged();
    lap("tree");
    logIfSlow(pid);
    scheduleChangesCount();   // most edits end in a refresh: recount what's unsent
}

QList<qint64> MainWindow::collectExpandedTaxa(const QModelIndex &parent) const
{
    QList<qint64> ids;
    const int rows = m_treeModel->rowCount(parent);
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = m_treeModel->index(r, 0, parent);
        if (m_treeView->isExpanded(idx)) {
            ids << idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong();
            ids += collectExpandedTaxa(idx);   // only descend into expanded nodes
        }
    }
    return ids;
}

void MainWindow::mutateTreePreservingState(const std::function<void()> &mutate)
{
    // Snapshot while the view is idle (nothing here forces a layout).
    const QList<qint64> expanded = collectExpandedTaxa();
    const QModelIndex topIdx = m_treeView->indexAt(QPoint(2, 2));
    const qint64 topTaxon = topIdx.isValid()
                                ? topIdx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                                : 0;
    const QModelIndex curIdx = m_treeView->currentIndex();
    const int curColumn = curIdx.isValid() ? curIdx.column() : 0;
    const qint64 curTaxon = curIdx.isValid()
                                ? curIdx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                                : m_lastSelectedTaxon;

    m_restoringTreeState = true;
    mutate();

    // Restore: expansions, then the current index, then scroll (last — scrollTo
    // forces the pending layout; nothing before it may).
    for (qint64 id : expanded) {
        const QModelIndex idx = m_treeModel->indexForTaxon(id);
        if (idx.isValid())
            m_treeView->setExpanded(idx, true);
    }
    if (curTaxon > 0) {
        const QModelIndex idx = m_treeModel->indexForTaxon(curTaxon);
        if (idx.isValid())
            m_treeView->setCurrentIndex(idx.sibling(idx.row(), curColumn));
    }
    if (const QModelIndex top = m_treeModel->indexForTaxon(topTaxon); top.isValid())
        m_treeView->scrollTo(top, QAbstractItemView::PositionAtTop);
    else if (m_treeView->currentIndex().isValid())
        m_treeView->scrollTo(m_treeView->currentIndex(), QAbstractItemView::EnsureVisible);

    m_restoringTreeState = false;
}

void MainWindow::applyThreatenedFilter(bool anyThreatened, const QString &status)
{
    QString normalisedStatus = status;
    const bool wasActive = m_treeModel->threatenedOnly() || !m_treeModel->statusFilter().isEmpty();

    // Toggle off: re-clicking the currently active tier/summary clears it.
    if (m_treeModel->threatenedOnly() == anyThreatened
        && m_treeModel->statusFilter() == normalisedStatus && wasActive) {
        anyThreatened = false;
        normalisedStatus.clear();
    }
    if (m_treeModel->threatenedOnly() == anyThreatened
        && m_treeModel->statusFilter() == normalisedStatus)
        return;

    // The introduced filter and the threatened ones replace each other.
    if ((anyThreatened || !normalisedStatus.isEmpty()) && m_treeModel->introducedOnly())
        applyIntroducedFilter(false);

    // Switching straight from one threatened filter to another: clear first,
    // exactly as "Clear filter" would, then apply the new one as a fresh
    // filter -- otherwise the tree keeps the previous category's expanded
    // branches and the new category's taxa stay folded away.
    if (wasActive && (anyThreatened || !normalisedStatus.isEmpty())) {
        applyThreatenedFilter(false, QString());
        applyThreatenedFilter(anyThreatened, normalisedStatus);
        return;
    }

    // mutateTreePreservingState() normally re-selects m_lastSelectedTaxon when
    // nothing is currently selected; that must not happen here, since "no
    // node selected" is exactly the trigger for the aggregate-scope path in
    // onTreeSelectionChanged().
    m_lastSelectedTaxon = 0;
    mutateTreePreservingState([this, anyThreatened, normalisedStatus] {
        m_treeModel->setThreatenedOnly(anyThreatened);
        m_treeModel->setStatusFilter(normalisedStatus);
    });

    const bool nowActive = anyThreatened || !normalisedStatus.isEmpty();
    if (nowActive) {
        // Reveal every taxon in the category: the pruned tree is small, so
        // unlike photographedOnly a full expand is cheap.
        m_treeView->expandAll();
        m_treeView->scrollToTop();
    }
    m_coveragePanel->setActiveStatusFilter(anyThreatened, normalisedStatus);
    onTreeSelectionChanged();
}

void MainWindow::applyIntroducedFilter(bool on)
{
    if (m_treeModel->introducedOnly() == on)
        return;
    // Replaces any threatened filter, cleared first as "Clear filter" would.
    if (on && (m_treeModel->threatenedOnly() || !m_treeModel->statusFilter().isEmpty()))
        applyThreatenedFilter(false, QString());

    m_lastSelectedTaxon = 0;   // see applyThreatenedFilter()
    mutateTreePreservingState([this, on] { m_treeModel->setIntroducedOnly(on); });
    if (on) {
        m_treeView->expandAll();
        m_treeView->scrollToTop();
    }
    m_coveragePanel->setActiveIntroducedFilter(on);
    onTreeSelectionChanged();
}

void MainWindow::rebuildRankFilterMenu()
{
    const QStringList ranks = m_treeModel->availableRanks();
    if (ranks == m_rankFilterOrder)
        return;   // same ranks as last time; leave the menu (and its open state) alone

    m_rankFilterOrder = ranks;
    m_rankFilterMenu->clear();
    m_rankCheckboxes.clear();

    for (const QString &rank : ranks) {
        auto *box = new QCheckBox(rank, m_rankFilterMenu);
        box->setChecked(!m_hiddenRanks.contains(rank));
        connect(box, &QCheckBox::toggled, this, [this, rank](bool on) {
            if (on)
                m_hiddenRanks.remove(rank);
            else
                m_hiddenRanks.insert(rank);
            mutateTreePreservingState([this] { m_treeModel->setHiddenRanks(m_hiddenRanks); });
        });
        auto *action = new QWidgetAction(m_rankFilterMenu);
        action->setDefaultWidget(box);
        m_rankFilterMenu->addAction(action);
        m_rankCheckboxes.insert(rank, box);
    }
}

void MainWindow::onTreeSelectionChanged()
{
    if (m_restoringTreeState)
        return;

    const QModelIndex idx = m_treeView->currentIndex();
    m_selectedTaxon = idx.isValid()
                          ? idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                          : 0;
    if (m_selectedTaxon > 0)
        m_lastSelectedTaxon = m_selectedTaxon;
    const int projectScope = currentProjectId();

    // With no node selected, an active threatened-status filter narrows the
    // photo tabs to the (possibly scattered) set of matching taxa instead of
    // "the whole tree" -- selecting a specific node always takes priority.
    const bool anyThreatened = m_treeModel->threatenedOnly();
    const QString statusFilter = m_treeModel->statusFilter();
    const bool introduced = m_treeModel->introducedOnly();
    const bool useStatusScope =
        m_selectedTaxon <= 0 && (anyThreatened || !statusFilter.isEmpty() || introduced);
    const QList<qint64> statusTaxa = !useStatusScope ? QList<qint64>()
                                     : introduced    ? m_treeModel->introducedTaxa()
                                     : anyThreatened ? m_treeModel->anyThreatenedTaxa()
                                                     : m_treeModel->taxaWithStatus(statusFilter);

    // Both "Photos of Tree Selection" and "My Best Shots" are confined to the
    // active reference tree; a taxon scope narrows them further to one branch.
    if (useStatusScope) {
        m_taxonModel->setTaxonSetScope(projectScope, statusTaxa);
        if (m_refPhotoModel)
            m_refPhotoModel->setTaxonSetScope(statusTaxa);
        if (m_bestShotModel)
            m_bestShotModel->setTaxonSetScope(projectScope, statusTaxa);
    } else {
        m_taxonModel->setScope(projectScope, m_selectedTaxon);
        if (m_refPhotoModel)
            m_refPhotoModel->setScope(m_selectedTaxon);
        if (m_bestShotModel)
            m_bestShotModel->setScope(projectScope, m_selectedTaxon);
    }
    if (m_reviewPane) {
        m_reviewPane->setTreeTaxon(
            useStatusScope ? 0 : m_selectedTaxon,
            (!useStatusScope && m_selectedTaxon > 0)
                ? idx.data(model::TaxonomyTreeModel::NameRole).toString()
                : QString());
    }
    if (m_inatView) {
        m_inatView->setTreeSelection(
            useStatusScope ? 0 : m_selectedTaxon,
            (!useStatusScope && m_selectedTaxon > 0)
                ? idx.data(model::TaxonomyTreeModel::NameRole).toString()
                : QString());
    }

    if (useStatusScope) {
        m_taxonInfo->setText(
            anyThreatened
                ? tr("<i>Showing every photo of a threatened species in this tree.</i>")
                : tr("<i>Showing every photo of a %1 species in this tree.</i>")
                      .arg(statusFilter.toHtmlEscaped()));
        m_taxonRepImage->clear();
        if (m_bestShotHeader)
            m_bestShotHeader->setText(
                anyThreatened
                    ? tr("Your best shots of threatened species in \"%1\".")
                          .arg(m_projectCombo->currentText())
                    : tr("Your best shots of %1 species in \"%2\".")
                          .arg(statusFilter, m_projectCombo->currentText()));
        return;
    }

    if (m_selectedTaxon <= 0) {
        m_taxonInfo->setText(projectScope > 0
                                 ? tr("<i>Showing every photo in this reference tree. "
                                      "Select a taxon to narrow it.</i>")
                                 : tr("<i>Select a taxon in the tree to see its photos.</i>"));
        m_taxonRepImage->clear();
        if (m_bestShotHeader)
            m_bestShotHeader->setText(
                projectScope > 0
                    ? tr("Your best shots of species in \"%1\". Select a taxon to narrow "
                         "this to one branch.").arg(m_projectCombo->currentText())
                    : tr("Every photo you've starred as a best shot of its species."));
        return;
    }

    const auto cov = m_coverage.byTaxon.value(m_selectedTaxon);
    if (m_bestShotHeader)
        m_bestShotHeader->setText(
            tr("Your best shots of %1 and everything below it.").arg(cov.name));
    QString html = QStringLiteral("<h3 style='margin:0'>%1</h3>").arg(cov.name.toHtmlEscaped());
    if (!cov.commonName.isEmpty() && cov.commonName != cov.name)
        html += QStringLiteral("<div>%1</div>").arg(cov.commonName.toHtmlEscaped());
    html += QStringLiteral("<div style='color:%1'>%2</div>")
                .arg(pl::themeColors(pl::currentThemeVariant()).mutedText.name(), cov.rank);
    if (!cov.status.isEmpty())
        html += QStringLiteral("<div><b>%1</b></div>").arg(cov.status.toHtmlEscaped());

    if (cov.speciesTotal > 0)
        html += QStringLiteral("<p>%1 of %2 species photographed</p>")
                    .arg(cov.speciesWithPhotos).arg(cov.speciesTotal);
    html += QStringLiteral("<p>%1</p>")
                .arg(cov.captureCount == 1 ? tr("1 capture")
                                           : tr("%1 captures").arg(cov.captureCount));
    if (!cov.newestCapture.isEmpty())
        html += QStringLiteral("<p style='color:%1'>Most recent: %2</p>")
                    .arg(pl::themeColors(pl::currentThemeVariant()).mutedText.name(), cov.newestCapture);
    m_taxonInfo->setText(html);

    const auto rep = coverage::representativeFor(m_app.database().connectionName(),
                                                 currentProjectId(), m_selectedTaxon);
    if (!rep.previewPath.isEmpty()) {
        const QPixmap pm = m_app.thumbnails().thumbnail(rep.previewHash, rep.previewPath,
                                                        thumb::ThumbnailCache::kGridPx);
        m_taxonRepImage->setPixmap(pm.isNull()
                                       ? QPixmap()
                                       : pm.scaled(m_taxonRepImage->size(), Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
    } else {
        m_taxonRepImage->setPixmap({});
        m_taxonRepImage->setText(tr("no photo"));
    }
}

namespace {

QString missingTaxonLabel(const coverage::TaxonCoverage &tc)
{
    QString label = tc.name;
    if (!tc.commonName.isEmpty() && tc.commonName != tc.name)
        label = QStringLiteral("%1  ·  %2").arg(tc.name, tc.commonName);
    if (!tc.status.isEmpty())
        label += QStringLiteral("   — %1").arg(tc.status);
    return label;
}

bool matchesFilter(const coverage::TaxonCoverage &tc, const QString &filter)
{
    return filter.isEmpty() || tc.name.contains(filter, Qt::CaseInsensitive)
        || tc.commonName.contains(filter, Qt::CaseInsensitive);
}

} // namespace

void MainWindow::updateMissingList()
{
    if (!m_missingList)
        return;
    m_missingList->clear();

    // Every leaf-rank taxon (species, or an infraspecific/hybrid rank below
    // it) with no photo anywhere in its own subtree.
    QList<coverage::TaxonCoverage> missing;
    for (const auto &tc : m_coverage.byTaxon) {
        if (taxonomy::TaxonomyStore::isLeafRank(tc.rank) && !tc.subtreeHasPhotos)
            missing.append(tc);
    }

    // Split into missing species (top-level rows) and missing infraspecific
    // taxa (nested under their parent species) -- e.g. a missing Acacia
    // verticillata var. ovoidea groups under Acacia verticillata even when
    // the species itself already has photos of the typical variety.
    QHash<qint64, coverage::TaxonCoverage> topLevel;   // by species inatId
    QHash<qint64, QList<coverage::TaxonCoverage>> childrenByParent;
    for (const auto &tc : missing) {
        if (taxonomy::TaxonomyStore::isInfraspecificRank(tc.rank) && tc.parentInatId > 0
            && m_coverage.byTaxon.contains(tc.parentInatId)) {
            childrenByParent[tc.parentInatId].append(tc);
        } else {
            topLevel.insert(tc.inatId, tc);
        }
    }
    // A species with a missing infraspecific child needs a row to hang it
    // off even when the species itself isn't missing (it has photos of a
    // different variety).
    for (auto it = childrenByParent.constBegin(); it != childrenByParent.constEnd(); ++it) {
        if (!topLevel.contains(it.key()))
            topLevel.insert(it.key(), m_coverage.byTaxon.value(it.key()));
    }

    QList<coverage::TaxonCoverage> topLevelList = topLevel.values();
    std::sort(topLevelList.begin(), topLevelList.end(),
              [](const coverage::TaxonCoverage &a, const coverage::TaxonCoverage &b) {
                  return a.name < b.name;
              });

    const QString filter = m_missingSearch ? m_missingSearch->text().trimmed() : QString();
    const QColor warning = pl::themeColors(pl::currentThemeVariant()).warning;
    for (const auto &tc : topLevelList) {
        QList<coverage::TaxonCoverage> children = childrenByParent.value(tc.inatId);
        std::sort(children.begin(), children.end(),
                  [](const coverage::TaxonCoverage &a, const coverage::TaxonCoverage &b) {
                      return a.name < b.name;
                  });

        const bool selfMatches = matchesFilter(tc, filter);
        QList<coverage::TaxonCoverage> visibleChildren;
        for (const auto &child : children) {
            if (selfMatches || matchesFilter(child, filter))
                visibleChildren.append(child);
        }
        if (!selfMatches && visibleChildren.isEmpty())
            continue;

        auto *parentItem = new QTreeWidgetItem(m_missingList);
        parentItem->setText(0, missingTaxonLabel(tc));
        parentItem->setData(0, Qt::UserRole, tc.inatId);
        if (!tc.status.isEmpty())
            parentItem->setForeground(0, warning);

        for (const auto &child : visibleChildren) {
            auto *childItem = new QTreeWidgetItem(parentItem);
            childItem->setText(0, missingTaxonLabel(child));
            childItem->setData(0, Qt::UserRole, child.inatId);
            if (!child.status.isEmpty())
                childItem->setForeground(0, warning);
        }
        parentItem->setExpanded(true);
    }

    const int missingTab = m_tabs->indexOf(m_missingList->parentWidget());
    if (missingTab >= 0)
        m_tabs->setTabText(missingTab, missing.isEmpty()
                                          ? tr("Unphotographed Taxa")
                                          : tr("Unphotographed Taxa (%1)").arg(missing.size()));
}

void MainWindow::reassignPhotos(QListView *grid, model::CaptureListModel *model)
{
    if (!ensureCanEdit())
        return;
    QList<int> captureIds;
    const QModelIndexList selected = grid->selectionModel()->selectedIndexes();
    if (!selected.isEmpty()) {
        for (const QModelIndex &idx : selected)
            captureIds << idx.data(model::CaptureListModel::IdRole).toInt();
    } else {
        const int rows = model->rowCount();
        if (rows == 0) {
            statusBar()->showMessage(tr("No photos to reassign."), 5000);
            return;
        }
        if (QMessageBox::question(
                this, tr("Reassign All Photos"),
                tr("Nothing is selected. Reassign all %1 photo(s) shown here to a "
                   "different taxon?")
                    .arg(rows))
            != QMessageBox::Yes)
            return;
        for (int r = 0; r < rows; ++r)
            captureIds << model->index(r, 0).data(model::CaptureListModel::IdRole).toInt();
    }

    TaxonPickerDialog picker(m_app.database().connectionName(), this);
    picker.setWindowTitle(tr("Reassign %1 Photo(s) To…").arg(captureIds.size()));
    if (picker.exec() != QDialog::Accepted)
        return;
    const qint64 newTaxonInatId = picker.chosenTaxonInatId();
    if (newTaxonInatId <= 0)
        return;

    match::MatchReviewer reviewer(m_app.database().connectionName());
    int ok = 0;
    for (int captureId : std::as_const(captureIds)) {
        if (reviewer.confirm(captureId, newTaxonInatId, true))
            ++ok;
    }

    m_taxonModel->reload();
    m_model->reload();
    m_bestShotModel->reload();
    if (model != m_taxonModel && model != m_model && model != m_bestShotModel)
        model->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();

    statusBar()->showMessage(
        tr("Reassigned %1 of %2 photo(s).").arg(ok).arg(captureIds.size()), 6000);
}

void MainWindow::applyCaptionFields(int fields)
{
    m_app.settings().setCaptureCaptionFields(fields);
    m_model->setCaptionFields(fields);
    if (m_taxonModel)
        m_taxonModel->setCaptionFields(fields);
    if (m_bestShotModel)
        m_bestShotModel->setCaptionFields(fields);
    if (m_untreedModel)
        m_untreedModel->setCaptionFields(fields);

    // Grow the grid cells to fit however many caption lines are now showing.
    // File Type doesn't count — it's drawn as an on-image badge, not a line.
    int lines = 0;
    for (int b = fields & ~model::CaptureListModel::CaptionFileType; b; b &= (b - 1))
        ++lines;
    lines = std::max(lines, 1);
    const QSize gridSize(212, 192 + 20 + lines * 16);
    if (m_grid)
        m_grid->setGridSize(gridSize);
    if (m_taxonGrid)
        m_taxonGrid->setGridSize(gridSize);
    if (m_bestShotGrid)
        m_bestShotGrid->setGridSize(gridSize);
    if (m_untreedGrid)
        m_untreedGrid->setGridSize(gridSize);
}

void MainWindow::updateReviewTabText()
{
    const int n = m_reviewPane->queueCount();
    m_viewReviewAction->setText(n > 0 ? tr("&Review Unmatched (%1)").arg(n) : tr("&Review Unmatched"));
}

void MainWindow::switchToTreeTab(QWidget *page)
{
    m_viewTreeAction->setChecked(true);
    m_tabs->setCurrentWidget(page);
}

void MainWindow::updateBestShotsTabText()
{
    if (!m_bestShotModel)
        return;
    const int tab = m_tabs->indexOf(m_bestShotGrid->parentWidget());
    if (tab < 0)
        return;
    const int n = m_bestShotModel->captureCount();
    m_tabs->setTabText(tab, n > 0 ? tr("My Best Shots (%1)").arg(n) : tr("My Best Shots"));
}

void MainWindow::openViewer(QAbstractItemModel *model, const QModelIndex &clicked)
{
    if (!model || !clicked.isValid())
        return;

    // A photo only online comes down first.
    if (CaptureDelegate::isOnlineOnly(clicked.data(model::CaptureListModel::PreviewPathRole).toString())) {
        const QStringList files =
            onlineOnlyRenditions({clicked.data(model::CaptureListModel::IdRole).toInt()});
        if (QMessageBox::question(this, tr("Photo Only Online"),
                                  tr("This photo is kept online, not on this computer. Download "
                                     "it now?\n\nIt then stays on this computer."))
            != QMessageBox::Yes)
            return;
        const QPointer<QAbstractItemModel> guard(model);
        const QPersistentModelIndex index(clicked);
        fetchPhotos(files, [this, guard, index] {
            if (guard && index.isValid()
                && QFileInfo::exists(index.data(model::CaptureListModel::PreviewPathRole).toString()))
                openViewer(guard, index);
        });
        return;
    }

    QVector<ImageViewer::Item> items;
    int start = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        ImageViewer::Item item;
        item.path = idx.data(model::CaptureListModel::PreviewPathRole).toString();
        item.title = idx.data(model::CaptureListModel::DisplayNameRole).toString();
        item.caption = idx.data(model::CaptureListModel::FullCaptionRole).toString();
        item.captureId = idx.data(model::CaptureListModel::IdRole).toInt();
        item.canBestShot =
            !idx.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty();
        item.bestShot = idx.data(model::CaptureListModel::IsBestShotRole).toBool();
        item.hasGps = idx.data(model::CaptureListModel::HasGpsRole).toBool();
        item.latitude = idx.data(model::CaptureListModel::LatitudeRole).toDouble();
        item.longitude = idx.data(model::CaptureListModel::LongitudeRole).toDouble();
        if (item.path.isEmpty())
            continue;
        if (r == clicked.row())
            start = items.size();
        items.append(item);
    }
    if (items.isEmpty())
        return;

    if (!m_viewer) {
        m_viewer = new ImageViewer(this);
        connect(m_viewer, &ImageViewer::bestShotToggleRequested, this,
                [this](qint64 captureId, bool nominate) {
                    if (captureId <= 0 || !ensureCanEdit())
                        return;
                    catalogue::BestShotStore store(m_app.database().connectionName());
                    if (store.setBestShots({int(captureId)}, nominate) < 0) {
                        statusBar()->showMessage(
                            tr("Could not update best shots: %1").arg(store.error()), 8000);
                        return;
                    }
                    const QList<int> ids{int(captureId)};
                    m_model->applyBestShot(ids, nominate);
                    m_taxonModel->applyBestShot(ids, nominate);
                    m_bestShotModel->reload();
                    updateBestShotsTabText();
                    m_viewer->markBestShot(captureId, nominate);
                    statusBar()->showMessage(nominate
                                                 ? tr("Added to your best shots.")
                                                 : tr("Removed from your best shots."),
                                             4000);
                });
    }
    // Show before loading the image: on a freshly constructed dialog that has
    // never been shown, resize() in the constructor only records the desired
    // top-level size -- children (including the scroll area's viewport) aren't
    // actually laid out to it until the widget is shown. Calling setItems()
    // first would make its rescale() read the viewport's stale, tiny
    // pre-layout size, scaling the first photo down to a thumbnail that stays
    // wrong until the viewer is reopened.
    m_viewer->show();
    m_viewer->raise();
    m_viewer->activateWindow();
    m_viewer->setItems(items, start);
}

bool MainWindow::ensureCatalogueReachable()
{
    if (m_app.settings().catalogueDescriptor().backend != CatalogueDescriptor::Backend::Postgres)
        return true;

    // Probed on a background thread, exactly like the startup connect flow
    // in main.cpp (Database::probeReachable() over a throwaway connection) --
    // the point is to let a suspended Neon compute wake up here, where the
    // GUI thread stays responsive the whole time, rather than during the
    // first real, synchronous write the caller is about to make, which would
    // block the GUI thread for however long that takes and can look exactly
    // like a hang to the OS.
    CatalogueConnectDialog dialog(this);
    dialog.hideUseLocalButton();
    dialog.setStatus(tr("Waking up the shared catalogue…"));
    dialog.show();

    struct ProbeResult
    {
        bool ok = false;
        QString error;
    };

    auto *watcher = new QFutureWatcher<ProbeResult>();
    QEventLoop waitLoop;
    ProbeResult result;
    connect(watcher, &QFutureWatcher<ProbeResult>::finished, &waitLoop, [&] {
        result = watcher->result();
        waitLoop.quit();
    });
    connect(watcher, &QFutureWatcher<ProbeResult>::finished, watcher, &QObject::deleteLater);

    const CatalogueDescriptor descriptor = m_app.settings().catalogueDescriptor();
    watcher->setFuture(QtConcurrent::run([descriptor] {
        ProbeResult r;
        r.ok = Database::probeReachable(descriptor, &r.error);
        return r;
    }));
    waitLoop.exec();
    dialog.close();

    if (!result.ok) {
        QMessageBox::warning(this, tr("Shared Catalogue Unreachable"),
                             tr("Could not reach the shared catalogue:\n\n%1").arg(result.error));
        return false;
    }
    return true;
}

void MainWindow::beginTaskProgress(const QString &title, std::function<void()> onCancel)
{
    delete m_taskProgress;
    m_taskProgress = new TaskProgressDialog(title, bool(onCancel), this);
    m_taskProgress->setWindowModality(Qt::ApplicationModal);
    m_taskProgress->setMinimumDuration(0);
    m_taskProgress->setAutoClose(false);
    m_taskProgress->setAutoReset(false);
    if (onCancel) {
        connect(m_taskProgress, &QProgressDialog::canceled, this,
                [onCancel] { onCancel(); });
    }
    m_taskProgress->show();
}

void MainWindow::updateTaskProgress(const QString &phase, int done, int total)
{
    if (!m_taskProgress)
        return;
    m_taskProgress->setLabelText(phase);
    if (total > 0) {
        m_taskProgress->setRange(0, total);
        // A modal QProgressDialog's setValue() processes events, so the
        // task's own finished signal can be delivered right here -- and its
        // handler calls endTaskProgress(), leaving m_taskProgress null.
        m_taskProgress->setValue(done);
        if (!m_taskProgress)
            return;
    } else {
        m_taskProgress->setRange(0, 0);   // busy/indeterminate
    }
    // QProgressDialog hides itself when its Cancel button is pressed, but the
    // task keeps running until it notices -- stay visible until
    // endTaskProgress() so the user can see it hasn't stopped yet.
    if (!m_taskProgress->isVisible())
        m_taskProgress->show();
}

void MainWindow::showTaskCancelling(const QString &label)
{
    if (!m_taskProgress)
        return;
    static_cast<TaskProgressDialog *>(m_taskProgress)->markCancelling();
    updateTaskProgress(label, 0, 0);
}

void MainWindow::endTaskProgress()
{
    if (!m_taskProgress)
        return;
    m_taskProgress->hide();   // not close(): TaskProgressDialog refuses to close by hand
    m_taskProgress->deleteLater();
    m_taskProgress = nullptr;
}

void MainWindow::refreshReferenceTree()
{
    if (!ensureCanEdit())
        return;
    if (m_builder->isRunning() || m_addingTaxon)
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Refresh Reference Tree"),
                                tr("Select a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT p.name, rt.name, rt.rank, pl.name "
        "FROM project p "
        "LEFT JOIN taxon rt ON rt.inat_id = p.root_taxon_inat_id "
        "LEFT JOIN place pl ON pl.inat_id = p.place_inat_id WHERE p.id = ?"));
    q.addBindValue(pid);
    if (!q.exec() || !q.next() || q.value(1).isNull()) {
        QMessageBox::information(this, tr("Refresh Reference Tree"),
                                tr("This tree can't be refreshed automatically."));
        return;
    }

    taxonomy::ProjectBuilder::Request request;
    request.projectName = q.value(0).toString();
    request.taxonQuery = q.value(1).toString();
    request.rank = q.value(2).toString();
    request.placeQuery = q.value(3).toString();

    m_refreshTreeAction->setEnabled(false);
    statusBar()->showMessage(tr("Refreshing reference tree…"));
    beginTaskProgress(tr("Refreshing Reference Tree"), [this] { m_builder->cancel(); });
    m_builder->start(request);
}

void MainWindow::addTaxonToReferenceTree()
{
    if (!ensureCanEdit())
        return;
    if (m_builder->isRunning() || m_addingTaxon)
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Add Taxon to Reference Tree"),
                                tr("Select a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    TaxonConfirmDialog dialog(m_app.inat(), QString(), QString(), this,
                              tr("Add Taxon to Reference Tree"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto chosen = dialog.chosenTaxon();
    if (!chosen)
        return;

    addTaxonToTree(pid, chosen->inatId, chosen->name);
}

void MainWindow::addTaxonToTree(int pid, qint64 inatId, const QString &name)
{
    if (m_builder->isRunning() || m_addingTaxon) {
        statusBar()->showMessage(
            tr("Wait for the current reference tree task to finish first."), 6000);
        return;
    }

    m_addingTaxon = true;
    m_addTaxonAction->setEnabled(false);
    m_newTreeAction->setEnabled(false);
    m_refreshTreeAction->setEnabled(false);
    m_deleteTreeAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching \"%1\" from iNaturalist…").arg(name));
    beginTaskProgress(tr("Fetching \"%1\" from iNaturalist…").arg(name));

    m_app.inat().fetchTaxon(inatId, [this, pid, name](pl::net::Outcome<pl::net::TaxonDetail> out) {
        m_addingTaxon = false;
        m_addTaxonAction->setEnabled(true);
        m_newTreeAction->setEnabled(true);
        m_refreshTreeAction->setEnabled(true);
        m_deleteTreeAction->setEnabled(true);
        endTaskProgress();

        if (!out.ok()) {
            statusBar()->showMessage(tr("Could not fetch \"%1\": %2").arg(name, out.error), 8000);
            return;
        }
        // The tree may have been deleted while the (async, non-modal) fetch
        // above was in flight. (Switching to another tree is fine: the taxon
        // still goes into the one it was meant for.)
        const int comboIndex = m_projectCombo->findData(pid);
        if (comboIndex < 0) {
            statusBar()->showMessage(
                tr("The reference tree was removed before \"%1\" could be added.").arg(name),
                8000);
            return;
        }
        const QString treeName = m_projectCombo->itemText(comboIndex);
        if (!m_app.taxonomyStore().addTaxonWithAncestors(pid, out.value.ancestors, out.value.taxon)) {
            QMessageBox::warning(this, tr("Add Taxon to Reference Tree"),
                                 tr("Could not add \"%1\".").arg(name));
            return;
        }

        if (pid == currentProjectId()) {
            mutateTreePreservingState([this, pid] { m_treeModel->setProject(pid); });
            refreshCoverage();
            updateMissingList();
            reloadProjectList();
        } else {
            reloadUntreedTaxa();
        }
        statusBar()->showMessage(tr("Added \"%1\" to \"%2\".").arg(name, treeName), 6000);
    });
}

void MainWindow::showTreeSelectorMenu(const QPoint &pos)
{
    if (currentProjectId() <= 0)
        return;
    QMenu menu(this);
    menu.addAction(tr("&Rename…"), this, &MainWindow::renameReferenceTree);
    menu.addSeparator();
    menu.addAction(m_refreshTreeAction);
    menu.addAction(tr("E&xport Reference Tree…"), this, &MainWindow::exportReferenceTree);
    menu.addSeparator();
    menu.addAction(m_deleteTreeAction);
    menu.exec(m_projectCombo->mapToGlobal(pos));
}

void MainWindow::renameReferenceTree()
{
    const int pid = currentProjectId();
    if (pid <= 0 || !ensureCanEdit())
        return;
    auto &store = m_app.taxonomyStore();
    const QString current = store.projectName(pid);
    QString name = current;
    while (true) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Rename Reference Tree"), tr("New name:"),
                                     QLineEdit::Normal, name, &ok)
                   .trimmed();
        if (!ok || name.isEmpty() || name == current)
            return;
        if (!store.projectIdByName(name))
            break;
        QMessageBox::information(this, tr("Rename Reference Tree"),
                                 tr("A reference tree called \"%1\" already exists. Choose "
                                    "another name.")
                                     .arg(name));
    }
    if (!store.renameProject(pid, name)) {
        QMessageBox::warning(this, tr("Rename Reference Tree"),
                             tr("Could not rename \"%1\".").arg(current));
        return;
    }
    reloadProjectList();   // keeps this tree selected (by id)
    statusBar()->showMessage(tr("Renamed \"%1\" to \"%2\".").arg(current, name), 6000);
}

void MainWindow::deleteReferenceTree()
{
    if (!ensureCanEdit())
        return;
    if (m_builder->isRunning() || m_addingTaxon)
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Delete Reference Tree"),
                                tr("Select a reference tree first."));
        return;
    }

    const QString name = m_projectCombo->currentText();
    const auto reply = QMessageBox::question(
        this, tr("Delete Reference Tree"),
        tr("Delete \"%1\"? This removes the tree and its coverage tracking. "
           "Your photos and any matches already made to its taxa are not affected.")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    taxonomy::TaxonomyStore store(m_app.database().connectionName());
    if (!store.deleteProject(pid)) {
        QMessageBox::warning(this, tr("Delete Reference Tree"),
                             tr("Could not delete \"%1\".").arg(name));
        return;
    }

    reloadProjectList();
    statusBar()->showMessage(tr("Deleted \"%1\".").arg(name), 6000);
}

void MainWindow::fetchInfraspecificTaxa()
{
    startInfraspecificFetch(0, QString());   // whole tree
}

void MainWindow::startInfraspecificFetch(qint64 scopeInatId, const QString &scopeName)
{
    if (!ensureCanEdit())
        return;
    if (m_infraFiller->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Fetch Subspecies/Varieties"),
                                tr("Select a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    const bool scoped = scopeInatId > 0;
    const QString where = scoped && !scopeName.isEmpty() ? scopeName
                                                         : m_projectCombo->currentText();

    taxonomy::TaxonomyStore store(m_app.database().connectionName());
    const int pendingCount =
        int(store.projectSpeciesNeedingInfraCheck(pid, scopeInatId).size());
    if (pendingCount == 0) {
        QMessageBox::information(
            this, tr("Fetch Subspecies/Varieties"),
            scoped ? tr("No species to check under \"%1\".").arg(where)
                   : tr("Every species in \"%1\" has already been checked.").arg(where));
        return;
    }

    const auto reply = QMessageBox::question(
        this, tr("Fetch Subspecies/Varieties"),
        tr("Check %1 species %2 \"%3\" for iNaturalist subspecies, varieties and hybrids? "
           "This makes a few requests per species, so it can be slow for a large branch. "
           "It's safe to quit and run again later.")
            .arg(pendingCount)
            .arg(scoped ? tr("under") : tr("in"))
            .arg(where),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_fetchInfraAction->setEnabled(false);
    statusBar()->showMessage(tr("Checking species for subspecies/varieties…"));
    beginTaskProgress(tr("Fetching Subspecies/Varieties"), [this] { m_infraFiller->cancel(); });
    m_infraFiller->start(pid, scopeInatId);
}

void MainWindow::showTreeContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_treeView->indexAt(pos);
    if (!idx.isValid())
        return;

    const qint64 inatId = idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong();
    if (inatId <= 0)
        return;
    const QString name = idx.data(model::TaxonomyTreeModel::NameRole).toString();

    // Right-click acts on the clicked row, so make it the selection too — the
    // coverage panel and the photo grids follow along.
    m_treeView->setCurrentIndex(idx.sibling(idx.row(), 0));

    QMenu menu(this);
    menu.setToolTipsVisible(true);

    QAction *infra = menu.addAction(tr("Fetch Subspecies/Varieties under \"%1\"…").arg(name));
    infra->setEnabled(!m_infraFiller->isRunning() && currentProjectId() > 0);
    connect(infra, &QAction::triggered, this,
            [this, inatId, name] { startInfraspecificFetch(inatId, name); });

    QAction *groupMatch = menu.addAction(tr("Match Library Against \"%1\"").arg(name));
    groupMatch->setToolTip(
        tr("Re-check unmatched photos, photos waiting in Review Unmatched, and genus-level "
           "matches against the taxa in this group only. Your confirmed decisions are never "
           "changed."));
    groupMatch->setEnabled(!m_app.matchService().isRunning() && currentProjectId() > 0);
    connect(groupMatch, &QAction::triggered, this,
            [this, inatId, name] { startGroupMatch(inatId, name); });

    menu.addSeparator();

    QAction *showPhotos = menu.addAction(tr("Show Photos of This Taxon"));
    connect(showPhotos, &QAction::triggered, this, [this, inatId] {
        selectTaxonInTree(inatId);
        switchToTreeTab(m_tabs->widget(0));   // Photos of Tree Selection
    });

    QAction *openInat = menu.addAction(tr("Open on iNaturalist"));
    connect(openInat, &QAction::triggered, this, [inatId] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(inatId)));
    });

    // Pruning the tree's own root isn't meaningful -- Delete Reference Tree
    // is the action for that -- so this is left off the menu for it.
    const auto rootId = m_app.taxonomyStore().projectRootTaxonInatId(currentProjectId());
    if (!rootId || *rootId != inatId) {
        menu.addSeparator();
        QAction *prune = menu.addAction(tr("Prune from Reference Tree…"));
        connect(prune, &QAction::triggered, this,
                [this, inatId, name] { pruneTaxonFromTree(inatId, name); });
    }

    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

void MainWindow::pruneTaxonFromTree(qint64 inatId, const QString &name)
{
    if (!ensureCanEdit())
        return;
    if (m_addingTaxon)
        return;
    const int pid = currentProjectId();
    if (pid <= 0)
        return;

    auto &store = m_app.taxonomyStore();
    const int count = store.projectPruneCount(pid, inatId);
    if (count <= 0)
        return;

    const auto reply = QMessageBox::question(
        this, tr("Prune from Reference Tree"),
        tr("Remove \"%1\" and %n other taxa from this tree? A future refresh "
           "won't bring them back. Photos and matches already made are not "
           "affected.",
           nullptr, count - 1)
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    if (store.pruneTaxonFromProject(pid, inatId) < 0) {
        QMessageBox::warning(this, tr("Prune from Reference Tree"),
                             tr("Could not prune \"%1\".").arg(name));
        return;
    }

    mutateTreePreservingState([this, pid] { m_treeModel->setProject(pid); });
    refreshCoverage();
    updateMissingList();
    reloadProjectList();
    statusBar()->showMessage(tr("Pruned \"%1\" from the tree.").arg(name), 6000);
}

void MainWindow::showMissingListContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_missingList->itemAt(pos);
    if (!item)
        return;
    const qint64 inatId = item->data(0, Qt::UserRole).toLongLong();
    if (inatId <= 0)
        return;

    QMenu menu(this);
    QAction *viewPhoto = menu.addAction(tr("View Reference Photo"));
    connect(viewPhoto, &QAction::triggered, this, [this, inatId] { viewReferencePhoto(inatId); });
    menu.exec(m_missingList->viewport()->mapToGlobal(pos));
}

void MainWindow::viewReferencePhoto(qint64 inatId)
{
    const auto photo = m_app.taxonomyStore().taxonPhoto(inatId);

    if (!m_referencePhotoDialog)
        m_referencePhotoDialog = new ReferencePhotoDialog(m_app.photoCache(), this);
    // Show before loading the photo: on a freshly constructed, never-shown
    // dialog, showTaxon()'s rescale() would read the image label's stale
    // pre-layout size (see the same fix in openViewer() for ImageViewer).
    m_referencePhotoDialog->show();
    m_referencePhotoDialog->raise();
    m_referencePhotoDialog->activateWindow();
    m_referencePhotoDialog->showTaxon(inatId, photo.name, photo.commonName, photo.photoUrl,
                                      photo.attribution);
}

void MainWindow::importChecklist()
{
    if (!ensureCanEdit())
        return;
    if (m_checklistImporter->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Import Checklist"),
                                tr("Select or create a reference tree to import into first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Checklist CSV"), QDir::homePath(),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Checklist"), tr("Could not read %1.").arg(path));
        return;
    }
    const auto entries = checklist::parseChecklistCsv(file.readAll());
    if (entries.isEmpty()) {
        QMessageBox::information(this, tr("Import Checklist"),
                                tr("No usable rows found in %1.").arg(QFileInfo(path).fileName()));
        return;
    }

    const QString source = QInputDialog::getText(
        this, tr("Import Checklist"), tr("Status source label:"), QLineEdit::Normal,
        QFileInfo(path).completeBaseName());

    checklist::ChecklistImporter::Request request;
    request.projectId = pid;
    request.source = source;
    request.entries = entries;

    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral("SELECT place_inat_id FROM project WHERE id = ?"));
    q.addBindValue(pid);
    if (q.exec() && q.next() && !q.value(0).isNull())
        request.placeInatId = q.value(0).toLongLong();

    m_importChecklistAction->setEnabled(false);
    statusBar()->showMessage(tr("Importing %1 checklist rows…").arg(entries.size()));
    beginTaskProgress(tr("Importing Checklist"), [this] { m_checklistImporter->cancel(); });
    m_checklistImporter->start(request);
}

void MainWindow::exportReferenceTree()
{
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Export Reference Tree"),
                                 tr("Select a reference tree to export first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    const QString treeName = m_projectCombo->currentText();
    const QString xlsxFilter = tr("Excel workbook (*.xlsx)");
    const QString csvFilter = tr("CSV file (*.csv)");
    QString selectedFilter = xlsxFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Reference Tree"),
        QDir(QDir::homePath()).filePath(util::sanitizeFilenameComponent(treeName)
                                        + QStringLiteral(".xlsx")),
        xlsxFilter + QStringLiteral(";;") + csvFilter, &selectedFilter);
    if (path.isEmpty())
        return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("xlsx") && suffix != QLatin1String("csv"))
        path += selectedFilter == csvFilter ? QStringLiteral(".csv") : QStringLiteral(".xlsx");

    const coverage::ProjectCoverage coverage =
        coverage::computeCoverage(m_app.database().connectionName(), pid);
    const treefile::TreeFile file =
        treefile::exportProject(m_app.taxonomyStore(), pid, treeName, &coverage);
    QString error;
    if (!treefile::writeTreeFile(path, file, &error)) {
        QMessageBox::warning(this, tr("Export Reference Tree"),
                             tr("Could not export the tree: %1").arg(error));
        return;
    }
    statusBar()->showMessage(tr("Exported %n taxa to %1.", nullptr, int(file.rows.size()))
                                 .arg(QFileInfo(path).fileName()),
                             8000);
}

void MainWindow::importReferenceTree()
{
    if (!ensureCanEdit())
        return;
    if (m_treeImporter->isRunning())
        return;
    if (!ensureCatalogueReachable())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Reference Tree"), QDir::homePath(),
        tr("Reference trees (*.xlsx *.csv);;Excel workbooks (*.xlsx);;CSV files (*.csv);;"
           "All files (*)"));
    if (path.isEmpty())
        return;

    QString error;
    const auto file = treefile::readTreeFile(path, &error);
    if (!file) {
        QMessageBox::warning(this, tr("Import Reference Tree"),
                             tr("Could not read %1: %2").arg(QFileInfo(path).fileName(), error));
        return;
    }

    int withoutId = 0;
    for (const auto &row : file->rows) {
        if (!row.inatId)
            ++withoutId;
    }
    QString summary = tr("%n taxa in this file.", nullptr, int(file->rows.size()));
    if (withoutId > 0)
        summary += QLatin1Char(' ')
                   + tr("%n without an iNaturalist ID will be looked up online.", nullptr,
                        withoutId);

    QString name = file->name.isEmpty() ? QFileInfo(path).completeBaseName() : file->name;
    while (true) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Import Reference Tree"),
                                     summary + QStringLiteral("\n\n")
                                         + tr("Name for the new reference tree:"),
                                     QLineEdit::Normal, name, &ok)
                   .trimmed();
        if (!ok || name.isEmpty())
            return;
        if (!m_app.taxonomyStore().projectIdByName(name))
            break;
        QMessageBox::information(this, tr("Import Reference Tree"),
                                 tr("A reference tree called \"%1\" already exists. "
                                    "Choose another name.")
                                     .arg(name));
        name += tr(" (imported)");
    }

    treefile::TreeImporter::Request request;
    request.treeName = name;
    request.file = *file;
    request.defaultSource = QFileInfo(path).completeBaseName();

    m_importTreeAction->setEnabled(false);
    statusBar()->showMessage(tr("Importing reference tree \"%1\"…").arg(name));
    beginTaskProgress(tr("Importing Reference Tree"), [this] { m_treeImporter->cancel(); });
    m_treeImporter->start(request);
}

void MainWindow::importLightroom()
{
    if (!ensureCanEdit())
        return;
    if (m_app.lightroomImporter().isRunning())
        return;
    if (currentProjectId() <= 0) {
        QMessageBox::information(this, tr("Import Lightroom Catalog"),
                                tr("Select or create a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Lightroom Catalog"), QDir::homePath(),
        tr("Lightroom catalogs (*.lrcat);;All files (*)"));
    if (path.isEmpty())
        return;

    m_importLightroomAction->setEnabled(false);
    statusBar()->showMessage(tr("Importing Lightroom catalog…"));
    beginTaskProgress(tr("Importing Lightroom Catalog"),
                      [this] { m_app.lightroomImporter().cancel(); });
    m_app.lightroomImporter().start(path);
}

void MainWindow::reloadProjectList()
{
    const int previous = m_projectCombo->currentData().isValid()
                             ? m_projectCombo->currentData().toInt()
                             : -1;

    QSignalBlocker block(m_projectCombo);
    m_projectCombo->clear();

    if (m_app.database().isOpen()) {
        QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
        q.exec(QStringLiteral(
            "SELECT p.id, p.name, "
            "  EXISTS(SELECT 1 FROM project_excluded_taxon pe WHERE pe.project_id = p.id) "
            "FROM project p ORDER BY p.name"));
        while (q.next()) {
            const QString name = q.value(1).toString();
            const bool pruned = q.value(2).toBool();
            m_projectCombo->addItem(pruned ? tr("%1 (customized)").arg(name) : name,
                                    q.value(0).toInt());
        }
    }

    if (m_projectCombo->count() > 0) {
        const int restore = m_projectCombo->findData(previous);
        m_projectCombo->setCurrentIndex(restore >= 0 ? restore : 0);
    }

    QList<std::pair<int, QString>> trees;
    for (int i = 0; i < m_projectCombo->count(); ++i)
        trees.append({m_projectCombo->itemData(i).toInt(), m_projectCombo->itemText(i)});
    m_inatView->setTrees(trees, currentProjectId());

    if (m_projectCombo->count() == 0) {
        m_treeModel->setProject(-1);
        refreshCoverage();
        return;
    }
    m_treeModel->setProject(m_projectCombo->currentData().toInt());
    m_treeView->expandToDepth(1);
    refreshCoverage();
}

void MainWindow::startMatch()
{
    if (!ensureCanEdit())
        return;
    if (m_app.matchService().isRunning() || m_matchAfterScanPending)
        return;
    if (m_model->captureCount() == 0 && m_app.settings().watchedRoots().isEmpty()) {
        QMessageBox::information(this, tr("Match Library"),
                                tr("Add and scan a folder of photos first."));
        return;
    }
    beginMatchProgress(tr("Match Library"));
    scanThenMatch([this] {
        m_groupMatchName.clear();
        m_matchingDownload = false;
        m_app.matchService().start();
    });
}

void MainWindow::scanThenMatch(std::function<void()> startMatching)
{
    // Matching reads the names recorded by the last scan, so a rename or
    // move the folder watcher hasn't caught up with yet (a renamed folder
    // especially) would otherwise be matched under its old name. Unchanged
    // files aren't re-read, so this scan is normally quick.
    if (m_app.settings().watchedRoots().isEmpty()) {
        startMatching();
        return;
    }
    m_matchAfterScanPending = true;
    m_matchAction->setEnabled(false);
    statusBar()->showMessage(tr("Checking the library for changes before matching…"));
    requestScan([this, startMatching = std::move(startMatching)] {
        m_matchAfterScanPending = false;
        m_matchAction->setEnabled(true);
        if (m_matchCancelled) {
            endMatchProgress();
            statusBar()->showMessage(tr("Matching cancelled."), 6000);
            return;
        }
        if (!m_lastScanCompleted) {
            endMatchProgress();
            statusBar()->showMessage(
                tr("Matching skipped — the library scan didn't finish, so some names could "
                   "be out of date."),
                10000);
            return;
        }
        if (m_app.matchService().isRunning()) {
            endMatchProgress();
            return;
        }
        startMatching();
    });
}

void MainWindow::beginMatchProgress(const QString &title)
{
    m_matchProgressActive = true;
    m_matchCancelled = false;
    beginTaskProgress(title, [this] {
        m_matchCancelled = true;
        if (m_app.scanService().isRunning())
            m_app.scanService().cancel();
        if (m_app.matchService().isRunning())
            m_app.matchService().cancel();
        showTaskCancelling(tr("Cancelling…"));
    });
    updateTaskProgress(m_app.settings().watchedRoots().isEmpty()
                           ? tr("Matching…")
                           : tr("Checking the library for changes…"),
                       0, 0);
}

void MainWindow::endMatchProgress()
{
    if (!m_matchProgressActive)
        return;
    m_matchProgressActive = false;
    endTaskProgress();
}

void MainWindow::resolveStaleDecisions(const QList<match::StaleDecision> &stale)
{
    StaleMatchDialog dialog(stale, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    match::MatchReviewer reviewer(m_app.database().connectionName());
    int moved = 0, kept = 0;
    for (const match::StaleDecision &s : dialog.toReassociate()) {
        if (reviewer.confirm(s.captureId, s.suggestedInatId, false))
            ++moved;
    }
    for (const match::StaleDecision &s : dialog.toKeep()) {
        if (reviewer.keepDecision(s.captureId))
            ++kept;
    }

    m_taxonModel->reload();
    m_model->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();
    statusBar()->showMessage(tr("Re-associated %1 renamed photo(s); kept %2 as identified.")
                                 .arg(moved)
                                 .arg(kept),
                             8000);
}

void MainWindow::startGroupMatch(qint64 inatId, const QString &name)
{
    if (!ensureCanEdit())
        return;
    if (m_app.matchService().isRunning() || m_matchAfterScanPending)
        return;
    const int pid = currentProjectId();
    if (pid <= 0 || !ensureCatalogueReachable())
        return;

    const QList<qint64> ids = m_app.taxonomyStore().projectSubtreeTaxonIds(pid, inatId);
    if (ids.isEmpty()) {
        statusBar()->showMessage(tr("\"%1\" has no taxa in this tree to match against.").arg(name),
                                 6000);
        return;
    }
    beginMatchProgress(tr("Match Library Against \"%1\"").arg(name));
    scanThenMatch([this, name, group = QSet<qint64>(ids.begin(), ids.end())] {
        m_groupMatchName = name;
        m_matchingDownload = false;
        m_app.matchService().start(group);
    });
}

void MainWindow::newReferenceTree()
{
    if (!ensureCanEdit())
        return;
    if (m_builder->isRunning())
        return;
    if (!ensureCatalogueReachable())
        return;

    NewProjectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    taxonomy::ProjectBuilder::Request request = dialog.request();

    if (!request.placeQuery.trimmed().isEmpty()) {
        PlaceConfirmDialog placeDialog(m_app.inat(), request.placeQuery, this);
        if (placeDialog.exec() != QDialog::Accepted)
            return;
        request.confirmedPlace = placeDialog.chosenPlace();
    }

    TaxonConfirmDialog taxonDialog(m_app.inat(), request.taxonQuery, request.rank, this);
    if (taxonDialog.exec() != QDialog::Accepted)
        return;
    request.confirmedTaxon = taxonDialog.chosenTaxon();

    m_newTreeAction->setEnabled(false);
    m_builderIsNewTree = true;
    statusBar()->showMessage(tr("Building reference tree…"));
    beginTaskProgress(tr("Building Reference Tree"), [this] { m_builder->cancel(); });
    m_builder->start(request);
}

namespace {

// Opens the platform file manager with `path` pre-selected, where the
// platform/desktop supports it. Falls through a few common Linux file
// managers' own "select" flags before giving up and just opening the
// containing folder.
void revealInFileManager(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    if (QProcess::startDetached(QStringLiteral("nautilus"), {QStringLiteral("--select"), path}))
        return;
    if (QProcess::startDetached(QStringLiteral("dolphin"), {QStringLiteral("--select"), path}))
        return;
    if (QProcess::startDetached(QStringLiteral("nemo"), {path}))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

} // namespace

QListView *MainWindow::makeCaptureGrid(QAbstractItemModel *model)
{
    auto *grid = new QListView(this);
    grid->setModel(model);
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust);
    grid->setMovement(QListView::Static);
    grid->setUniformItemSizes(true);
    grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);   // read-only grid; no renaming
    grid->setIconSize(QSize(192, 192));
    grid->setGridSize(QSize(212, 232));
    grid->setWordWrap(true);
    grid->setSpacing(6);

    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid, &QWidget::customContextMenuRequested, this, [this, grid](const QPoint &pos) {
        const QModelIndex idx = grid->indexAt(pos);
        if (!idx.isValid())
            return;
        // Right-click doesn't extend a selection: if the clicked photo isn't
        // already selected, act on it alone.
        if (!grid->selectionModel()->isSelected(idx))
            grid->setCurrentIndex(idx);

        const QModelIndexList sel = grid->selectionModel()->selectedIndexes();
        const int selected = sel.size();
        const QString path = idx.data(model::CaptureListModel::PreviewPathRole).toString();

        int nBest = 0, nMatched = 0;
        for (const QModelIndex &s : sel) {
            if (s.data(model::CaptureListModel::IsBestShotRole).toBool())
                ++nBest;
            if (!s.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty())
                ++nMatched;
        }

        QMenu menu(grid);
        menu.setToolTipsVisible(true);
        QAction *reveal = menu.addAction(tr("Open in File Manager"));
        reveal->setEnabled(!path.isEmpty());
        connect(reveal, &QAction::triggered, this, [path] { revealInFileManager(path); });
        QList<int> onlineIds;
        for (const QModelIndex &s : sel) {
            if (CaptureDelegate::isOnlineOnly(s.data(model::CaptureListModel::PreviewPathRole).toString()))
                onlineIds << s.data(model::CaptureListModel::IdRole).toInt();
        }
        if (!onlineIds.isEmpty()) {
            QAction *download = menu.addAction(
                tr("Download to This Computer (%Ln)", nullptr, int(onlineIds.size())));
            download->setToolTip(tr("These photos are kept online only; download them and keep "
                                    "them here"));
            connect(download, &QAction::triggered, this, [this, onlineIds] {
                fetchPhotos(onlineOnlyRenditions(onlineIds), {});
            });
        }

        menu.addSeparator();
        QAction *addBest = menu.addAction(
            selected == 1 && nMatched == 1
                ? tr("Add to Best Shots of %1")
                      .arg(idx.data(model::CaptureListModel::MatchedNameRole).toString())
                : tr("Add to Best Shot of Species"));
        addBest->setEnabled(nMatched > nBest);
        connect(addBest, &QAction::triggered, this,
                [this, grid] { setBestShotForSelection(grid, true); });

        QAction *removeBest = menu.addAction(tr("Remove from Best Shot of Species"));
        removeBest->setEnabled(nBest > 0);
        connect(removeBest, &QAction::triggered, this,
                [this, grid] { setBestShotForSelection(grid, false); });

        menu.addSeparator();
        QAction *fetchCoords = menu.addAction(tr("Attempt to fetch coordinates from iNat"));
        // Exclude "pending" -- MatchEngine only leaves a match pending when
        // its confidence fell below the auto-apply threshold, i.e. an
        // uncertain guess that hasn't been reviewed yet, and searching
        // iNaturalist under the wrong species (a real catalogue taxon, just
        // not this photo's) reliably turns up no observations. "auto" is
        // fine: it already means a high-confidence match against the real
        // catalogue taxonomy, not merely raw filename text; "confirmed"
        // means a human verified or reassigned it.
        const QString matchStatus = idx.data(model::CaptureListModel::MatchStatusRole).toString();
        const bool canFetchCoords =
            selected == 1 && !idx.data(model::CaptureListModel::HasGpsRole).toBool()
            && idx.data(model::CaptureListModel::MatchedTaxonInatIdRole).toLongLong() > 0
            && matchStatus != QLatin1String("pending")
            && !idx.data(model::CaptureListModel::CapturedOnRole).toString().isEmpty()
            && !idx.data(model::CaptureListModel::PreviewIsRawRole).toBool();
        fetchCoords->setEnabled(canFetchCoords);
        if (!canFetchCoords)
            fetchCoords->setToolTip(tr("Available for a single, JPEG-backed photo with an "
                                       "auto or confirmed species identification (not a "
                                       "pending, unreviewed guess), a captured date, and no "
                                       "GPS yet."));
        connect(fetchCoords, &QAction::triggered, this,
                [this, idx] { fetchCoordinatesFromInat(idx); });

        menu.addSeparator();
        QAction *remove = menu.addAction(
            selected > 1 ? tr("Remove %1 Photos from PhotoLife…").arg(selected)
                         : tr("Remove Photo from PhotoLife…"));
        connect(remove, &QAction::triggered, this, [this, grid] { removeSelectedCaptures(grid); });

        menu.exec(grid->viewport()->mapToGlobal(pos));
    });

    return grid;
}

void MainWindow::setBestShotForSelection(QListView *grid, bool nominate)
{
    if (!ensureCanEdit())
        return;
    QList<int> ids;
    int skipped = 0;
    const QModelIndexList selected = grid->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected) {
        const int id = idx.data(model::CaptureListModel::IdRole).toInt();
        if (id <= 0 || ids.contains(id))
            continue;
        if (nominate
            && idx.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty()) {
            ++skipped;
            continue;
        }
        ids.append(id);
    }
    if (ids.isEmpty()) {
        if (skipped > 0)
            statusBar()->showMessage(
                tr("Identify a photo before adding it to your best shots."), 6000);
        return;
    }

    catalogue::BestShotStore store(m_app.database().connectionName());
    const int changed = store.setBestShots(ids, nominate);
    if (changed < 0) {
        statusBar()->showMessage(
            tr("Could not update best shots: %1").arg(store.error()), 8000);
        return;
    }

    m_model->applyBestShot(ids, nominate);
    m_taxonModel->applyBestShot(ids, nominate);
    m_bestShotModel->reload();
    updateBestShotsTabText();

    if (nominate) {
        QString msg = tr("Added %n photo(s) to your best shots.", nullptr, int(ids.size()));
        if (skipped > 0)
            msg += QLatin1Char(' ')
                   + tr("%n unidentified photo(s) skipped.", nullptr, skipped);
        statusBar()->showMessage(msg, 6000);
    } else {
        statusBar()->showMessage(
            tr("Removed %n photo(s) from your best shots.", nullptr, int(ids.size())), 6000);
    }
}

void MainWindow::removeSelectedCaptures(QListView *grid)
{
    QList<int> ids;
    const QModelIndexList selected = grid->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected) {
        const int id = idx.data(model::CaptureListModel::IdRole).toInt();
        if (id > 0 && !ids.contains(id))
            ids.append(id);
    }
    if (ids.isEmpty())
        return;

    const auto reply = QMessageBox::question(
        this, tr("Remove from PhotoLife"),
        tr("Remove %n photo(s) from PhotoLife's catalogue?\n\n"
           "No files are deleted from your drive — this only drops the catalogue "
           "entry, its thumbnail and any match. A rescan re-imports photos whose "
           "files are still present.",
           nullptr, int(ids.size())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int removed = maint.forgetCaptures(ids);
    if (removed <= 0) {
        statusBar()->showMessage(
            tr("Could not remove the selected photos: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();
    statusBar()->showMessage(
        tr("Removed %n photo(s) from the catalogue.", nullptr, removed), 6000);
}

void MainWindow::fetchCoordinatesFromInat(const QModelIndex &idx)
{
    const QString username = m_app.settings().inatUsername();
    if (username.isEmpty()) {
        QMessageBox::information(
            this, tr("Attempt to Fetch Coordinates"),
            tr("Set up your iNaturalist username in the iNaturalist view "
               "(iNaturalist Tools → iNaturalist Account) first."));
        return;
    }
    // The shared INatClient's token is otherwise only ever applied by the
    // iNaturalist view's account bar -- it can't be assumed already set.
    m_app.inat().setAccessToken(m_app.settings().inatApiToken());

    const int captureId = idx.data(model::CaptureListModel::IdRole).toInt();
    const qint64 taxonInatId =
        idx.data(model::CaptureListModel::MatchedTaxonInatIdRole).toLongLong();
    // Never search iNaturalist under a low-confidence, unreviewed guess --
    // see the same guard on the context-menu action for why.
    if (taxonInatId <= 0
        || idx.data(model::CaptureListModel::MatchStatusRole).toString()
               == QLatin1String("pending"))
        return;
    const QDate capturedOn = QDate::fromString(
        idx.data(model::CaptureListModel::CapturedOnRole).toString().left(10), Qt::ISODate);
    const QString path = idx.data(model::CaptureListModel::PreviewPathRole).toString();

    InatCoordinateConfirmDialog dialog(m_app.inat(), m_app.photoCache(), taxonInatId, username,
                                       capturedOn, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto chosen = dialog.chosenObservation();
    if (!chosen || !chosen->latitude || !chosen->longitude)
        return;

    inat::ExifFields fields;
    fields.latitude = chosen->latitude;
    fields.longitude = chosen->longitude;
    QString error;
    if (!inat::writeExifSafely(path, fields, &error)) {
        QMessageBox::warning(this, tr("Attempt to Fetch Coordinates"),
                             tr("Could not write coordinates into %1:\n%2").arg(path, error));
        return;
    }

    // Re-read rather than trust the values just written, so the catalogue
    // reflects exactly what's now actually in the file.
    const scan::ExifData reread = scan::readJpegExif(path);
    if (!reread.hasGps()) {
        statusBar()->showMessage(
            tr("Coordinates were written to the file but could not be re-read."), 8000);
        return;
    }

    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral("UPDATE capture SET latitude = ?, longitude = ? WHERE id = ?"));
    q.addBindValue(*reread.latitude);
    q.addBindValue(*reread.longitude);
    q.addBindValue(captureId);
    q.exec();

    m_model->applyGps(captureId, *reread.latitude, *reread.longitude);
    m_taxonModel->applyGps(captureId, *reread.latitude, *reread.longitude);
    m_bestShotModel->applyGps(captureId, *reread.latitude, *reread.longitude);
    statusBar()->showMessage(tr("Coordinates added from iNaturalist."), 6000);
}

void MainWindow::removeMissingCaptures()
{
    if (!ensureCanEdit())
        return;
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Remove Missing Photos"),
                                tr("Wait for the current scan to finish first."));
        return;
    }

    scan::CatalogueMaintenance maint(m_app.database().connectionName());

    QApplication::setOverrideCursor(Qt::WaitCursor);
    // Photos kept online only (photo sync) aren't missing.
    const QList<scan::CatalogueMaintenance::MissingCapture> missing =
        maint.capturesWithMissingFiles(CaptureDelegate::onlineOnly());
    QApplication::restoreOverrideCursor();

    if (missing.isEmpty()) {
        QMessageBox::information(
            this, tr("Remove Missing Photos"),
            tr("Every catalogued photo's file is still on your drive."));
        return;
    }

    // Show exactly which entries would go; every one is ticked, and the user can
    // untick any they want to keep.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remove Missing Photos"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(
        tr("%n catalogued photo(s) point at a file that is no longer on your drive "
           "— usually left behind when a file was renamed or moved.\n\n"
           "Ticked entries will be removed from PhotoLife's catalogue. No files are "
           "deleted from your drive.",
           nullptr, int(missing.size())),
        &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *list = new QListWidget(&dlg);
    list->setAlternatingRowColors(true);
    for (const auto &mc : missing) {
        const QString what = mc.missingPaths.isEmpty()
                                 ? QDir(mc.folderPath).filePath(mc.baseName)
                                 : mc.missingPaths.join(QStringLiteral("   +   "));
        auto *item = new QListWidgetItem(what, list);
        item->setToolTip(what);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, mc.captureId);
    }
    layout->addWidget(list, 1);

    auto *buttons = new QHBoxLayout;
    auto *checkAll = new QPushButton(tr("Tick All"), &dlg);
    auto *checkNone = new QPushButton(tr("Untick All"), &dlg);
    connect(checkAll, &QPushButton::clicked, &dlg, [list] {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Checked);
    });
    connect(checkNone, &QPushButton::clicked, &dlg, [list] {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Unchecked);
    });
    buttons->addWidget(checkAll);
    buttons->addWidget(checkNone);
    buttons->addStretch(1);

    auto *box = new QDialogButtonBox(&dlg);
    QPushButton *removeButton =
        box->addButton(tr("Remove Ticked"), QDialogButtonBox::AcceptRole);
    box->addButton(QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto updateRemoveButton = [list, removeButton] {
        int ticked = 0;
        for (int i = 0; i < list->count(); ++i)
            ticked += list->item(i)->checkState() == Qt::Checked ? 1 : 0;
        removeButton->setEnabled(ticked > 0);
        removeButton->setText(tr("Remove %n Ticked", nullptr, ticked));
    };
    connect(list, &QListWidget::itemChanged, &dlg, updateRemoveButton);
    updateRemoveButton();
    buttons->addWidget(box);
    layout->addLayout(buttons);

    dlg.resize(720, 420);
    if (dlg.exec() != QDialog::Accepted)
        return;

    QList<int> ids;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked)
            ids.append(list->item(i)->data(Qt::UserRole).toInt());
    }
    if (ids.isEmpty())
        return;

    const int removed = maint.forgetCaptures(ids);
    if (removed <= 0) {
        statusBar()->showMessage(
            tr("Could not remove the missing photos: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();
    statusBar()->showMessage(
        tr("Removed %n photo(s) whose files were missing.", nullptr, removed), 6000);
}

void MainWindow::fixRawGeolocation()
{
    if (!ensureCanEdit())
        return;
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Fix RAW Photo Locations"),
                                 tr("Wait for the current scan to finish first."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Fix RAW Photo Locations"),
            tr("Some RAW photos may have an incorrect GPS location due to a bug that has "
               "since been fixed (their hemisphere sign was sometimes lost, e.g. a Victoria "
               "photo ending up near Japan). This re-reads location data from each RAW file "
               "directly and corrects it in the catalogue. Continue?"))
        != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int updated = maint.backfillRawGeolocation();
    QApplication::restoreOverrideCursor();

    if (!maint.error().isEmpty()) {
        statusBar()->showMessage(
            tr("Fix RAW Photo Locations failed: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    statusBar()->showMessage(
        tr("Updated GPS location for %n RAW photo(s).", nullptr, updated), 6000);
}

void MainWindow::fetchPhotoLocalities()
{
    if (!ensureCanEdit())
        return;
    if (m_localityFetcher->isRunning())
        return;

    m_fetchLocalitiesAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching photo localities from OpenStreetMap…"));
    m_localityFetcher->start();
}

void MainWindow::buildCentralWidget()
{
    m_grid = makeCaptureGrid(m_model);
    m_grid->setItemDelegate(new CaptureDelegate(m_grid));
    connect(m_grid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_model, i); });

    m_emptyHint = new QLabel(this);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setEnabled(false);

    m_photoStack = new QStackedWidget(this);
    m_photoStack->addWidget(m_emptyHint);   // index 0
    m_photoStack->addWidget(m_grid);        // index 1

    m_reviewPane = new ReviewPane(m_app.database(), m_app.thumbnails(), m_app.inat(),
                                  m_app.photoCache(), this);
    connect(m_reviewPane, &ReviewPane::queueChanged, this, [this] {
        m_model->reload();
        if (m_bestShotModel)
            m_bestShotModel->reload();
        updateEmptyState();
        refreshCoverage();
        updateReviewTabText();
        updateBestShotsTabText();
    });

    m_inatView = new InatToolsView(m_app, this);
    connect(m_inatView->downloadPage(), &InatDownloadPage::statusMessage, this,
            [this](const QString &text, int timeoutMs) {
                statusBar()->showMessage(text, timeoutMs);
            });
    connect(m_inatView->downloadPage(), &InatDownloadPage::downloadRequested, this,
            &MainWindow::downloadSelectedInatPhotos);

    InatComparePage *comparePage = m_inatView->comparePage();
    connect(comparePage, &InatComparePage::statusMessage, this,
            [this](const QString &text, int timeoutMs) {
                statusBar()->showMessage(text, timeoutMs);
            });
    connect(comparePage, &InatComparePage::addToTreeRequested, this,
            [this](int projectId, qint64 inatId, const QString &name) {
                addTaxonToTree(projectId, inatId, name);
            });
    connect(comparePage, &InatComparePage::showInTreeRequested, this,
            [this](int projectId, qint64 inatId) {
                const int index = m_projectCombo->findData(projectId);
                if (index >= 0 && index != m_projectCombo->currentIndex())
                    m_projectCombo->setCurrentIndex(index);
                m_viewTreeAction->setChecked(true);
                selectTaxonInTree(inatId);
            });
    connect(comparePage, &InatComparePage::downloadTaxonRequested, this,
            [this](qint64 inatId, const QString &label) {
                m_inatView->downloadPage()->limitToTaxon(inatId, label);
                m_inatView->showDownloadPage();
            });

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("treeTabs"));   // main.cpp's --screenshot finds it by name
    // "Photos of Tree Selection" must stay tab 0: several call sites jump to
    // it via m_tabs->widget(0) rather than by name/index lookup.
    m_tabs->addTab(buildBrowsePage(), tr("Photos of Tree Selection"));
    m_tabs->addTab(buildMapPage(), tr("Map"));
    m_tabs->addTab(buildBestShotsPage(), tr("My Best Shots"));
    m_tabs->addTab(buildMissingPage(), tr("Unphotographed Taxa"));
    m_tabs->addTab(buildUntreedPage(), tr("Not in Any Tree"));
    m_tabs->addTab(buildReferencePhotosPage(), tr("Reference Photos"));

    m_centralStack = new QStackedWidget(this);
    m_centralStack->addWidget(m_tabs);        // Reference Tree mode
    m_centralStack->addWidget(m_photoStack);  // All Library Photos mode
    m_centralStack->addWidget(m_reviewPane);  // Review Unmatched mode
    m_centralStack->addWidget(m_inatView);    // iNaturalist mode
    setCentralWidget(m_centralStack);

    connect(m_viewTreeAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_tabs);
    });
    connect(m_viewLibraryAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_photoStack);
    });
    connect(m_viewReviewAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_reviewPane);
    });
    connect(m_viewInatAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_inatView);
    });
    m_viewLibraryAction->setChecked(true);   // start on All Photos

    updateReviewTabText();
    updateBestShotsTabText();
    updateReferencePhotoStatus();

    // Filling in missing reference photos is a network round-trip, so only do
    // it when the user actually opens the tab.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (m_refPhotoPage && m_tabs->widget(i) == m_refPhotoPage)
            maybeFetchReferencePhotos();
    });

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

QWidget *MainWindow::buildBrowsePage()
{
    m_taxonModel = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);

    m_taxonRepImage = new QLabel(this);
    m_taxonRepImage->setFixedSize(120, 120);
    m_taxonRepImage->setAlignment(Qt::AlignCenter);
    m_taxonRepImage->setFrameShape(QFrame::StyledPanel);

    m_taxonInfo = new QLabel(this);
    m_taxonInfo->setTextFormat(Qt::RichText);
    m_taxonInfo->setWordWrap(true);
    m_taxonInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto *reassignButton = new QPushButton(tr("Reassign to Taxon…"), this);
    reassignButton->setToolTip(
        tr("Move the selected photos (or, with nothing selected, every photo shown here) "
           "to a different taxon."));
    connect(reassignButton, &QPushButton::clicked, this,
            [this] { reassignPhotos(m_taxonGrid, m_taxonModel); });

    auto *header = new QHBoxLayout;
    header->addWidget(m_taxonRepImage);
    header->addWidget(m_taxonInfo, 1);
    header->addWidget(reassignButton, 0, Qt::AlignTop);

    m_taxonGrid = makeCaptureGrid(m_taxonModel);
    m_taxonGrid->setItemDelegate(new CaptureDelegate(m_taxonGrid));
    connect(m_taxonGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_taxonModel, i); });

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(header);
    layout->addWidget(m_taxonGrid, 1);
    return page;
}

QWidget *MainWindow::buildMapPage()
{
    m_mapView = new MapView(*m_taxonModel, m_app.tileCache(), this);
    m_mapView->setShowLocality(m_app.settings().showMapLocality());
    connect(m_mapView, &MapView::captureActivated, this,
            [this](const QModelIndex &i) { openViewer(m_taxonModel, i); });
    return m_mapView;
}

QWidget *MainWindow::buildBestShotsPage()
{
    m_bestShotModel = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_bestShotModel->setBestShotOnly(true);
    // setScope() (driven by tree clicks) reloads asynchronously, so the many
    // explicit updateBestShotsTabText() calls after a sync reload() aren't
    // enough on their own -- this catches every reset, sync or async.
    connect(m_bestShotModel, &QAbstractItemModel::modelReset, this,
            &MainWindow::updateBestShotsTabText);

    m_bestShotHeader = new QLabel(this);
    m_bestShotHeader->setWordWrap(true);
    m_bestShotHeader->setText(
        tr("Photos you've starred as a best shot of their species. Right-click a "
           "photo in any grid to add or remove it."));

    m_bestShotGrid = makeCaptureGrid(m_bestShotModel);
    m_bestShotGrid->setItemDelegate(new CaptureDelegate(m_bestShotGrid));
    connect(m_bestShotGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_bestShotModel, i); });

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_bestShotHeader);
    layout->addWidget(m_bestShotGrid, 1);
    return page;
}

QWidget *MainWindow::buildMissingPage()
{
    m_missingSearch = new QLineEdit(this);
    m_missingSearch->setPlaceholderText(tr("Filter by common or scientific name…"));
    m_missingSearch->setClearButtonEnabled(true);
    connect(m_missingSearch, &QLineEdit::textChanged, this, &MainWindow::updateMissingList);

    m_missingList = new QTreeWidget(this);
    m_missingList->setHeaderHidden(true);
    m_missingList->setAlternatingRowColors(true);
    connect(m_missingList, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        selectTaxonInTree(item->data(0, Qt::UserRole).toLongLong());
        switchToTreeTab(m_tabs->widget(0));   // Browse
    });
    m_missingList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_missingList, &QWidget::customContextMenuRequested, this,
            &MainWindow::showMissingListContextMenu);

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_missingSearch);
    layout->addWidget(m_missingList, 1);
    return page;
}

namespace {

// Rows of the "Not in Any Tree" tab's Show combo (m_untreedMode).
enum UntreedMode {
    UntreedNotInAnyTree = 0,
    UntreedNotInSelectedTree = 1,
    UntreedAboveSpecies = 2,
};

} // namespace

QWidget *MainWindow::buildUntreedPage()
{
    m_untreedHeader = new QLabel(this);
    m_untreedHeader->setWordWrap(true);

    m_untreedSearch = new QLineEdit(this);
    m_untreedSearch->setPlaceholderText(tr("Filter by common or scientific name…"));
    m_untreedSearch->setClearButtonEnabled(true);
    connect(m_untreedSearch, &QLineEdit::textChanged, this, &MainWindow::updateUntreedList);

    m_untreedMode = new QComboBox(this);
    m_untreedMode->addItem(tr("Taxa not in any tree"));             // UntreedNotInAnyTree
    m_untreedMode->addItem(tr("Taxa not in the selected tree"));    // UntreedNotInSelectedTree
    m_untreedMode->addItem(tr("Identified only to genus or higher"));   // UntreedAboveSpecies
    connect(m_untreedMode, &QComboBox::currentIndexChanged, this, &MainWindow::reloadUntreedTaxa);

    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Show:"), this));
    controls->addWidget(m_untreedMode);
    controls->addWidget(m_untreedSearch, 1);

    m_untreedList = new QTreeWidget(this);
    m_untreedList->setColumnCount(3);
    m_untreedList->setHeaderLabels({tr("Taxon"), tr("Rank"), tr("Photos")});
    m_untreedList->setRootIsDecorated(false);
    m_untreedList->setAlternatingRowColors(true);
    m_untreedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_untreedList->setSortingEnabled(true);
    m_untreedList->sortByColumn(0, Qt::AscendingOrder);
    // Taxon and Rank are user-draggable (a Stretch/ResizeToContents section
    // can't be resized by hand); Photos, the last column, takes up the slack.
    m_untreedList->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_untreedList->header()->setStretchLastSection(true);
    m_untreedList->header()->resizeSection(0, 240);
    m_untreedList->header()->resizeSection(1, 85);
    m_untreedList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_untreedList, &QWidget::customContextMenuRequested, this,
            &MainWindow::showUntreedListContextMenu);
    connect(m_untreedList, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::updateUntreedGridScope);

    // Text/tooltip set per mode by reloadUntreedTaxa(): "Add to Selected Tree",
    // or "Reassign to Taxon…" when listing genus-or-higher identifications.
    m_untreedAddButton = new QPushButton(this);
    connect(m_untreedAddButton, &QPushButton::clicked, this, [this] {
        if (m_untreedMode->currentIndex() == UntreedAboveSpecies) {
            reassignPhotos(m_untreedGrid, m_untreedModel);
            return;
        }
        const QList<QTreeWidgetItem *> selected = m_untreedList->selectedItems();
        if (selected.size() == 1)
            addUntreedTaxonToTree(selected.first(), currentProjectId());
    });

    auto *listPanel = new QWidget(this);
    auto *listLayout = new QVBoxLayout(listPanel);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->addWidget(m_untreedList, 1);
    listLayout->addWidget(m_untreedAddButton);

    m_untreedModel = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_untreedModel->setStatusFilter(QStringLiteral("confirmedOrAuto"));
    m_untreedGrid = makeCaptureGrid(m_untreedModel);
    m_untreedGrid->setItemDelegate(new CaptureDelegate(m_untreedGrid));
    connect(m_untreedGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_untreedModel, i); });

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(listPanel);
    split->addWidget(m_untreedGrid);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_untreedHeader);
    layout->addLayout(controls);
    layout->addWidget(split, 1);
    return page;   // populated by refreshCoverage() -> reloadUntreedTaxa()
}

void MainWindow::reloadUntreedTaxa()
{
    if (!m_untreedList)
        return;

    const int pid = currentProjectId();
    const int mode = m_untreedMode->currentIndex();
    m_untreedTaxa.clear();
    if (m_app.database().isOpen()) {
        catalogue::UntreedMatchStore store(m_app.database().connectionName());
        if (mode == UntreedAboveSpecies)
            m_untreedTaxa = store.aboveSpeciesTaxa();
        else if (mode == UntreedNotInSelectedTree && pid > 0)
            m_untreedTaxa = store.untreedTaxa(pid);
        else if (mode == UntreedNotInAnyTree)
            m_untreedTaxa = store.untreedTaxa();
        // (UntreedNotInSelectedTree with no tree selected: nothing to compare against.)
        if (!store.error().isEmpty())
            statusBar()->showMessage(tr("Could not list matched taxa: %1").arg(store.error()), 8000);
    }

    QString base;
    switch (mode) {
    case UntreedNotInSelectedTree:
        base = tr("Not in This Tree");
        m_untreedHeader->setText(
            tr("Photos matched to a taxon that isn't in the selected reference tree (other "
               "trees may have it). Select taxa to see their photos; right-click one to add "
               "it to a tree."));
        break;
    case UntreedAboveSpecies:
        base = tr("Genus or Higher");
        m_untreedHeader->setText(
            tr("Photos identified only to genus (or family, order…), not to a species — "
               "whether or not the taxon is in a tree. Select taxa to see their photos, then "
               "use Reassign to narrow photos you've since identified down to a species."));
        break;
    default:
        base = tr("Not in Any Tree");
        m_untreedHeader->setText(
            tr("Photos matched to a taxon that isn't in any of your reference trees — "
               "for example a subspecies or variety a tree doesn't list, or a species "
               "outside every tree's scope. Select taxa to see their photos; right-click "
               "one to add it to a tree."));
        break;
    }

    if (mode == UntreedAboveSpecies) {
        m_untreedAddButton->setText(tr("Reassign to Taxon…"));
        m_untreedAddButton->setToolTip(
            tr("Reassign the photos selected in the grid (or, with none selected, every "
               "photo shown) to a different taxon."));
    } else {
        m_untreedAddButton->setText(tr("Add to Selected Tree"));
        m_untreedAddButton->setToolTip(
            tr("Add the highlighted taxon (and its ancestors) to the reference tree selected "
               "on the left. Right-click a taxon to add it to a different tree."));
    }

    const int tab = m_tabs ? m_tabs->indexOf(m_untreedHeader->parentWidget()) : -1;
    if (tab >= 0)
        m_tabs->setTabText(tab, m_untreedTaxa.isEmpty()
                                    ? base
                                    : QStringLiteral("%1 (%2)").arg(base).arg(m_untreedTaxa.size()));

    updateUntreedList();
}

void MainWindow::updateUntreedList()
{
    if (!m_untreedList)
        return;

    QSet<qint64> previouslySelected;
    for (QTreeWidgetItem *item : m_untreedList->selectedItems())
        previouslySelected.insert(item->data(0, Qt::UserRole).toLongLong());

    const QString filter = m_untreedSearch->text().trimmed();

    {
        // One grid reload for the whole rebuild, not one per cleared/restored row.
        QSignalBlocker block(m_untreedList);
        m_untreedList->setSortingEnabled(false);
        m_untreedList->clear();
        for (const catalogue::UntreedTaxon &t : m_untreedTaxa) {
            if (!filter.isEmpty() && !t.name.contains(filter, Qt::CaseInsensitive)
                && !t.commonName.contains(filter, Qt::CaseInsensitive))
                continue;

            auto *item = new QTreeWidgetItem(m_untreedList);
            item->setText(0, !t.commonName.isEmpty() && t.commonName != t.name
                                 ? QStringLiteral("%1  ·  %2").arg(t.name, t.commonName)
                                 : t.name);
            item->setToolTip(0, item->text(0));
            item->setData(0, Qt::UserRole, t.inatId);
            item->setData(0, Qt::UserRole + 1, t.name);
            item->setText(1, t.rank);
            item->setData(2, Qt::DisplayRole, t.photoCount);
            item->setToolTip(2, tr("%1 confirmed, %2 auto-applied")
                                    .arg(t.confirmedCount)
                                    .arg(t.photoCount - t.confirmedCount));
            if (previouslySelected.contains(t.inatId))
                item->setSelected(true);
        }
        m_untreedList->setSortingEnabled(true);
    }
    updateUntreedGridScope();
}

void MainWindow::updateUntreedGridScope()
{
    if (!m_untreedModel)
        return;

    // No selection shows every listed taxon's photos at once.
    QList<QTreeWidgetItem *> items = m_untreedList->selectedItems();
    if (items.isEmpty()) {
        for (int i = 0; i < m_untreedList->topLevelItemCount(); ++i)
            items << m_untreedList->topLevelItem(i);
    }
    m_untreedAddButton->setEnabled(
        m_untreedMode->currentIndex() == UntreedAboveSpecies
            ? !items.isEmpty()
            : m_untreedList->selectedItems().size() == 1 && currentProjectId() > 0);

    // An empty taxon set means "no narrowing" to CaptureListModel (the whole
    // library), so hide the grid instead when nothing is listed.
    m_untreedGrid->setVisible(!items.isEmpty());
    if (items.isEmpty())
        return;

    QList<qint64> ids;
    ids.reserve(items.size());
    for (QTreeWidgetItem *item : items)
        ids << item->data(0, Qt::UserRole).toLongLong();
    std::sort(ids.begin(), ids.end());   // stable order, so an unchanged set doesn't reload
    m_untreedModel->setTaxonSetScope(0, ids);
}

void MainWindow::showUntreedListContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_untreedList->itemAt(pos);
    if (!item)
        return;
    const qint64 inatId = item->data(0, Qt::UserRole).toLongLong();

    QMenu menu(this);
    QMenu *addMenu = menu.addMenu(tr("Add to Reference Tree"));
    const int current = currentProjectId();
    // Current tree first, then the rest in the combo's (alphabetical) order.
    QList<int> order;
    if (m_projectCombo->findData(current) >= 0)
        order << m_projectCombo->findData(current);
    for (int i = 0; i < m_projectCombo->count(); ++i) {
        if (!order.contains(i))
            order << i;
    }
    for (int i : order) {
        const int pid = m_projectCombo->itemData(i).toInt();
        QString label = m_projectCombo->itemText(i);
        if (pid == current)
            label = tr("%1 (selected)").arg(label);
        QAction *add = addMenu->addAction(label);
        connect(add, &QAction::triggered, this,
                [this, item, pid] { addUntreedTaxonToTree(item, pid); });
        if (i == order.first() && pid == current && order.size() > 1)
            addMenu->addSeparator();
    }
    addMenu->setEnabled(!order.isEmpty());

    QAction *viewPhoto = menu.addAction(tr("View Reference Photo"));
    connect(viewPhoto, &QAction::triggered, this, [this, inatId] { viewReferencePhoto(inatId); });
    menu.exec(m_untreedList->viewport()->mapToGlobal(pos));
}

void MainWindow::addUntreedTaxonToTree(QTreeWidgetItem *item, int projectId)
{
    if (!ensureCanEdit())
        return;
    if (!item || projectId <= 0)
        return;
    const qint64 inatId = item->data(0, Qt::UserRole).toLongLong();
    const QString name = item->data(0, Qt::UserRole + 1).toString();
    if (!ensureCatalogueReachable())
        return;
    addTaxonToTree(projectId, inatId, name);
}

QWidget *MainWindow::buildReferencePhotosPage()
{
    m_refPhotoModel = new model::ReferencePhotoModel(m_app.database(), m_app.photoCache(), this);

    m_refPhotoStatus = new QLabel(this);
    m_refPhotoStatus->setWordWrap(true);

    auto *fetchButton = new QPushButton(tr("Fetch Reference Photos"), this);
    fetchButton->setToolTip(
        tr("Download a representative photo for each species in this reference tree "
           "from iNaturalist."));
    connect(fetchButton, &QPushButton::clicked, this, &MainWindow::fetchReferencePhotos);

    auto *header = new QHBoxLayout;
    header->addWidget(m_refPhotoStatus, 1);
    header->addWidget(fetchButton, 0, Qt::AlignTop);

    m_refPhotoGrid = new QListView(this);
    m_refPhotoGrid->setModel(m_refPhotoModel);
    m_refPhotoGrid->setViewMode(QListView::IconMode);
    m_refPhotoGrid->setResizeMode(QListView::Adjust);
    m_refPhotoGrid->setMovement(QListView::Static);
    m_refPhotoGrid->setUniformItemSizes(true);
    m_refPhotoGrid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_refPhotoGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_refPhotoGrid->setIconSize(QSize(160, 160));
    m_refPhotoGrid->setGridSize(QSize(188, 220));
    m_refPhotoGrid->setWordWrap(true);
    m_refPhotoGrid->setSpacing(6);
    connect(m_refPhotoGrid, &QAbstractItemView::doubleClicked, this, [](const QModelIndex &idx) {
        const qint64 inatId = idx.data(model::ReferencePhotoModel::InatIdRole).toLongLong();
        if (inatId > 0)
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(inatId)));
    });
    connect(m_refPhotoModel, &QAbstractItemModel::modelReset, this,
            &MainWindow::updateReferencePhotoStatus);

    m_refPhotoPage = new QWidget(this);
    auto *layout = new QVBoxLayout(m_refPhotoPage);
    layout->addLayout(header);
    layout->addWidget(m_refPhotoGrid, 1);
    return m_refPhotoPage;
}

void MainWindow::updateReferencePhotoStatus()
{
    if (!m_refPhotoStatus || !m_refPhotoModel)
        return;

    const int total = m_refPhotoModel->speciesCount();
    const int withPhoto = m_refPhotoModel->withPhotoCount();
    if (total == 0) {
        m_refPhotoStatus->setText(
            tr("Select a reference tree — or a taxon within it — to see its species' "
               "iNaturalist reference photos."));
    } else {
        m_refPhotoStatus->setText(
            tr("%1 of %2 species have a reference photo from iNaturalist. "
               "Double-click a photo to open the taxon on iNaturalist.")
                .arg(withPhoto)
                .arg(total));
    }

    const int tab = m_tabs->indexOf(m_refPhotoPage);
    if (tab >= 0)
        m_tabs->setTabText(tab, total > 0 ? tr("Reference Photos (%1)").arg(total)
                                          : tr("Reference Photos"));
}

void MainWindow::maybeFetchReferencePhotos()
{
    const int pid = currentProjectId();
    if (pid <= 0 || !m_refPhotoFetcher || m_refPhotoFetcher->isRunning())
        return;
    if (m_app.taxonomyStore().projectLeafTaxaMissingPhoto(pid).isEmpty())
        return;

    if (m_fetchRefPhotosAction)
        m_fetchRefPhotosAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching reference photos from iNaturalist…"));
    m_refPhotoFetcher->start(pid);
}

void MainWindow::fetchReferencePhotos()
{
    if (!ensureCanEdit())
        return;
    if (m_refPhotoFetcher && m_refPhotoFetcher->isRunning())
        return;

    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Fetch Reference Photos"),
                                tr("Select a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;
    if (m_app.taxonomyStore().projectLeafTaxaMissingPhoto(pid).isEmpty()) {
        QMessageBox::information(
            this, tr("Fetch Reference Photos"),
            tr("Every species in \"%1\" already has a reference photo.")
                .arg(m_projectCombo->currentText()));
        return;
    }

    switchToTreeTab(m_refPhotoPage);
    if (m_fetchRefPhotosAction)
        m_fetchRefPhotosAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching reference photos from iNaturalist…"));
    beginTaskProgress(tr("Fetching Reference Photos"), [this] { m_refPhotoFetcher->cancel(); });
    m_refPhotoFetcher->start(pid);
}

void MainWindow::exportReferenceTreePhotos()
{
    if (m_app.exportService().isRunning())
        return;

    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Export Photo Collection"),
                                tr("Select a reference tree first."));
        return;
    }
    if (!ensureCatalogueReachable())
        return;

    ExportDialog dialog(m_app.database(), m_app.thumbnails(), pid, m_projectCombo->currentText(),
                       m_treeModel->visibleTaxonIds(), m_treeModel->photographedOnly(),
                       m_app.settings().exportDestFolder(),
                       m_app.settings().exportCollectionFolder(),
                       m_app.settings().exportIncludeRaw(),
                       m_app.settings().exportThumbnailsOnly(), this);
    dialog.setFileNaming(m_app.settings().exportRenameFiles(),
                         m_app.settings().exportFileNamePattern(),
                         m_app.settings().exportAuthorName());
    if (dialog.exec() != QDialog::Accepted)
        return;
    collection::ExportOptions options = dialog.options();
    m_app.settings().setExportIncludeRaw(options.includeRaw);
    // The naming pattern is kept even when an update used its collection's
    // own, so the next new collection starts from the user's choice.
    m_app.settings().setExportRenameFiles(dialog.renamesFiles());
    m_app.settings().setExportFileNamePattern(dialog.fileNamePattern());
    m_app.settings().setExportAuthorName(dialog.authorName());
    // Update mode fixes this to the collection's own choice, so only a new
    // collection's choice is remembered.
    if (!dialog.updatesExistingCollection())
        m_app.settings().setExportThumbnailsOnly(options.thumbnailPx > 0);
    options.thumbnailCacheDir = m_app.thumbnails().cacheDir();
    m_exportThumbnails = options.thumbnailPx > 0;

    const QString destRoot = dialog.destRoot();
    if (dialog.updatesExistingCollection()) {
        m_app.settings().setExportCollectionFolder(destRoot);
    } else {
        if (!QDir().mkpath(destRoot)) {
            QMessageBox::warning(this, tr("Export Photo Collection"),
                                 tr("Could not create %1.").arg(destRoot));
            return;
        }
        m_app.settings().setExportDestFolder(destRoot);
    }

    auto start = [this, pid, destRoot, options] {
        m_exportTreePhotosAction->setEnabled(false);
        m_exportCancelling = false;
        statusBar()->showMessage(tr("Exporting photos…"));
        beginTaskProgress(tr("Exporting Photo Collection"), [this] {
            m_exportCancelling = true;
            m_app.exportService().cancel();
            showTaskCancelling(tr("Cancelling — finishing the file currently being copied…"));
        });
        updateTaskProgress(tr("Checking the destination folder…"), 0, 0);
        m_app.exportService().start(pid, destRoot, options);
    };
    // Photos only online have to come down first.
    if (const QStringList online = onlineOnlyRenditionsOfTree(pid); !online.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Export Photo Collection"),
            tr("%Ln of this tree's photo files are kept online, not on this computer. Download "
               "them before exporting?\n\nOtherwise the export leaves them out.",
               nullptr, int(online.size())),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Yes) {
            fetchPhotos(online, start);
            return;
        }
    }
    start();
}

void MainWindow::showExportProblems(const collection::ExportSummary &summary)
{
    QMessageBox box(QMessageBox::Warning, tr("Export Photo Collection"), QString(),
                    QMessageBox::Ok, this);
    if (!summary.ok()) {
        box.setText(tr("The export didn't finish: %1").arg(summary.error));
        box.setInformativeText(
            tr("%n photo(s) were copied before it stopped. Exporting again later picks up "
               "where it left off — photos already there are skipped.",
               nullptr, summary.photosCopied));
    } else {
        box.setText(tr("%n photo(s) couldn't be copied.", nullptr, summary.photosFailed));
        box.setInformativeText(
            tr("Everything else was exported. Exporting again later retries the ones that "
               "failed — photos already there are skipped."));
    }
    if (!summary.failures.isEmpty())
        box.setDetailedText(summary.failures.join(QLatin1Char('\n')));
    box.exec();
}

void MainWindow::showUnexpectedExportFiles(const collection::ExportSummary &summary)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Collection Updated"));
    dialog.resize(640, 420);

    auto *intro = new QLabel(
        tr("%n file(s) in the collection aren't part of this export — photos reassigned "
           "to another taxon or excluded since the last export, or files added by hand. "
           "Nothing was deleted; tidy them up yourself if they're no longer wanted.",
           nullptr, int(summary.unexpectedFiles.size())),
        &dialog);
    intro->setWordWrap(true);

    auto *list = new QPlainTextEdit(summary.unexpectedFiles.join(QLatin1Char('\n')), &dialog);
    list->setReadOnly(true);
    list->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *open = buttons->addButton(tr("Open Collection Folder"),
                                           QDialogButtonBox::ActionRole);
    const QString root = summary.collectionRoot;
    connect(open, &QPushButton::clicked, &dialog,
            [root] { QDesktopServices::openUrl(QUrl::fromLocalFile(root)); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(intro);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::downloadSelectedInatPhotos(const QList<inat::ImportItem> &items)
{
    if (!ensureCanEdit())
        return;
    if (m_inatImportService->isRunning() || items.isEmpty())
        return;

    const QString destParent = QFileDialog::getExistingDirectory(
        this, tr("Choose a Folder for Downloaded Photos"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (destParent.isEmpty())
        return;

    // A dedicated, timestamped subfolder, so this run's downloads are clearly
    // grouped and never collide with anything already at the chosen location.
    const QString destFolder = QDir(destParent).filePath(
        QStringLiteral("iNaturalist-%1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (!QDir().mkpath(destFolder)) {
        QMessageBox::warning(this, tr("Download Selected"),
                             tr("Could not create %1.").arg(destFolder));
        return;
    }

    m_inatPendingDestFolder = destFolder;
    m_inatPendingItems = items;
    m_inatView->downloadPage()->setDownloadEnabled(false);
    statusBar()->showMessage(tr("Downloading %n photo(s)…", nullptr, items.size()));

    // Locks the rest of the app until the whole pipeline -- download, add to
    // the library, match -- is done; anything else touching the library
    // meanwhile could interrupt it.
    m_inatPipelineActive = true;
    m_inatPipelineCancelled = false;
    beginTaskProgress(tr("Downloading from iNaturalist"), [this] { cancelInatPipeline(); });
    updateTaskProgress(tr("Downloading photo 1 of %1…").arg(items.size()), 0, int(items.size()));
    m_inatImportService->start(items, destFolder);
}

void MainWindow::onInatImportFinished(bool ok, const QString &error, QList<inat::SavedFile> saved)
{
    m_inatView->downloadPage()->setDownloadEnabled(true);
    const bool cancelled = !ok && error == QLatin1String("cancelled");

    // Leave whatever didn't arrive (skipped after retries, or never reached)
    // selected, so trying again is just Download Selected… once more.
    QSet<qint64> savedPhotoIds;
    for (const inat::SavedFile &s : std::as_const(saved))
        savedPhotoIds.insert(s.photoId);
    QSet<qint64> notDownloaded;
    for (const inat::ImportItem &item : std::as_const(m_inatPendingItems)) {
        if (!savedPhotoIds.contains(item.photoId))
            notDownloaded.insert(item.photoId);
    }
    m_inatView->downloadPage()->selectPhotos(notDownloaded);

    const QList<inat::FailedDownload> failures = m_inatImportService->failures();
    if (!cancelled && !notDownloaded.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, tr("Download from iNaturalist"),
                        ok ? tr("%n photo(s) couldn't be downloaded, even after retrying.", nullptr,
                                int(failures.size()))
                           : tr("Downloading stopped: %1 photos in a row couldn't be downloaded, "
                                "so the connection may be down.\n\nLast error: %2")
                                 .arg(inat::InatImportService::kMaxConsecutiveFailures)
                                 .arg(error),
                        QMessageBox::Ok, this);
        box.setInformativeText(
            tr("%n photo(s) weren't downloaded. They're still selected on the Download My "
               "Observations tab — click Download Selected… to try them again.",
               nullptr, int(notDownloaded.size()))
            + (saved.isEmpty() ? QString()
                               : QStringLiteral("\n\n")
                                     + tr("The %n photo(s) that did download are being added "
                                          "to your library.",
                                          nullptr, int(saved.size()))));
        QStringList details;
        for (const inat::FailedDownload &f : failures)
            details << QStringLiteral("%1 (photo %2): %3")
                           .arg(f.item.taxonName)
                           .arg(f.item.photoId)
                           .arg(f.error);
        box.setDetailedText(details.join(QLatin1Char('\n')));
        box.exec();
    }

    if (saved.isEmpty()) {
        finishInatPipeline(cancelled ? tr("Download cancelled — no photos were downloaded.")
                           : ok && failures.isEmpty()
                               ? tr("Nothing was downloaded.")
                               : tr("No photos could be downloaded from iNaturalist."));
        return;
    }

    statusBar()->showMessage(
        tr("Downloaded %n photo(s) — adding \"%1\" to the library…", nullptr, saved.size())
            .arg(m_inatPendingDestFolder),
        6000);
    updateTaskProgress(tr("Adding %n downloaded photo(s) to your library…", nullptr,
                          int(saved.size())),
                       0, 0);

    // Reuses addWatchedFolder()'s own tail: register the new folder, then
    // scan it so its files become real capture rows.
    QStringList roots = m_app.settings().watchedRoots();
    const QString clean = QDir::cleanPath(m_inatPendingDestFolder);
    if (!roots.contains(clean)) {
        roots.append(clean);
        m_app.settings().setWatchedRoots(roots);
        m_app.libraryWatcher().setRoots(roots);
    }
    updateEmptyState();

    // Provenance can only be stamped once a scan has created the capture rows
    // for these new files. requestScan() guarantees a scan that starts after
    // the last download landed -- the folder watcher has usually kicked one
    // off mid-download already, which would miss the final files.
    const QString connectionName = m_app.database().connectionName();
    const int count = int(saved.size());
    requestScan([this, connectionName, saved, count] {
        inat::InatImportService::stampProvenance(connectionName, saved);

        if (m_inatPipelineCancelled) {
            finishInatPipeline(
                tr("Cancelled — %n downloaded photo(s) added to the library but not matched. "
                   "Run Match Library to match them.",
                   nullptr, count));
            return;
        }

        // Match just the downloaded photos, not the whole library.
        const QList<qint64> captureIds =
            inat::InatImportService::captureIdsFor(connectionName, saved);
        if (captureIds.isEmpty()) {
            finishInatPipeline(tr("%n photo(s) from iNaturalist added to the library.", nullptr,
                                  count));
            return;
        }
        if (m_app.matchService().isRunning()) {
            finishInatPipeline(
                tr("%n photo(s) from iNaturalist added to the library. Match Library is "
                   "already running — run it again afterwards to match them.",
                   nullptr, count));
            return;
        }
        updateTaskProgress(tr("Matching the downloaded photos…"), 0, 0);
        m_groupMatchName.clear();
        m_matchingDownload = true;
        m_app.matchService().startForCaptures(captureIds);
    });
}

void MainWindow::cancelInatPipeline()
{
    if (!m_inatPipelineActive || m_inatPipelineCancelled)
        return;
    m_inatPipelineCancelled = true;

    if (m_inatImportService->isRunning()) {
        m_inatImportService->cancel();
        showTaskCancelling(tr("Cancelling — finishing the photo currently downloading, then "
                              "adding what's already downloaded to your library…"));
    } else if (m_matchingDownload && m_app.matchService().isRunning()) {
        m_app.matchService().cancel();
        showTaskCancelling(tr("Cancelling matching…"));
    } else if (m_app.scanService().isRunning()) {
        m_app.scanService().cancel();
        showTaskCancelling(tr("Cancelling the library scan…"));
    }
}

void MainWindow::finishInatPipeline(const QString &message)
{
    if (m_inatPipelineActive) {
        m_inatPipelineActive = false;
        m_inatPipelineCancelled = false;
        endTaskProgress();
    }
    statusBar()->showMessage(message, 10000);
}

void MainWindow::selectTaxonInTree(qint64 inatId)
{
    QModelIndex idx = m_treeModel->indexForTaxon(inatId);
    if (!idx.isValid() && m_photographedOnly->isChecked()) {
        // A missing (unphotographed) taxon is hidden by the filter — lift it,
        // keeping the tree's position, then try again.
        m_photographedOnly->setChecked(false);
        idx = m_treeModel->indexForTaxon(inatId);
    }
    if (!idx.isValid())
        return;
    m_treeView->setCurrentIndex(idx);
    m_treeView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void MainWindow::onTreeSearchChanged(const QString &text)
{
    m_treeSearchHits = m_treeModel->findTaxa(text);
    m_treeSearchPos = 0;
    if (!m_treeSearchHits.isEmpty())
        selectTaxonInTree(m_treeSearchHits.first());
}

void MainWindow::onTreeSearchNext()
{
    if (m_treeSearchHits.isEmpty())
        return;
    m_treeSearchPos = (m_treeSearchPos + 1) % m_treeSearchHits.size();
    selectTaxonInTree(m_treeSearchHits.at(m_treeSearchPos));
}

void MainWindow::updateEmptyState()
{
    QStackedWidget *stack = m_photoStack;
    if (!stack)
        return;

    const int count = m_model->captureCount();
    const bool filtered = !m_model->statusFilter().isEmpty();
    if (count > 0) {
        stack->setCurrentIndex(1);
    } else if (filtered) {
        m_emptyHint->setText(tr("No captures match this filter."));
        stack->setCurrentIndex(0);
    } else {
        const bool haveRoots = !m_app.settings().watchedRoots().isEmpty();
        m_emptyHint->setText(haveRoots
                                 ? tr("No photos catalogued yet. Rescan the library to import them.")
                                 : tr("Add a folder of photos to start building your library."));
        stack->setCurrentIndex(0);
    }

    m_statusLabel->setText(count > 0
                               ? tr("%n capture(s)%1", nullptr, count)
                                     .arg(filtered ? tr(" (filtered)") : QString())
                               : QString());
}

void MainWindow::addWatchedFolder()
{
    if (!ensureCanEdit())
        return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add Folder to Library"),
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty())
        return;

    QStringList roots = m_app.settings().watchedRoots();
    const QString clean = QDir::cleanPath(dir);
    // A server catalogue's photos must be in its shared library folder, or
    // collaborators can't open them.
    if (m_app.settings().syncEnabled()) {
        const QString root = QDir::cleanPath(m_app.settings().libraryRoot());
        bool outside = false;
        sync::PathMapping(root).toServer(clean, &outside);
        if (outside
            && QMessageBox::warning(
                   this, tr("Add Folder to Library"),
                   tr("\"%1\" isn't inside this catalogue's photo library folder (%2), so your "
                      "collaborators won't be able to open its photos.\n\nAdd it anyway?")
                       .arg(QDir::toNativeSeparators(clean), QDir::toNativeSeparators(root)),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes)
            return;
    }
    if (!roots.contains(clean)) {
        roots.append(clean);
        m_app.settings().setWatchedRoots(roots);
        m_app.libraryWatcher().setRoots(roots);
    }

    updateEmptyState();

    // Only this folder needs scanning -- CatalogueWriter::sync() never
    // deletes or prunes anything outside the roots it's given, so this
    // can't affect captures already catalogued under the other watched
    // roots. Scanning the whole library here (the way startScan()/"Rescan
    // Library" does) would make adding one small folder wait on a full
    // re-walk of everything else for no benefit.
    if (m_app.scanService().isRunning())
        return;
    m_app.scanService().start({clean});
    beginScanProgress(tr("Adding \"%1\" to the Library").arg(QFileInfo(clean).fileName()));
}

void MainWindow::manageLibraryFolders()
{
    if (!ensureCanEdit())
        return;
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Manage Library Folders"),
                                 tr("Wait for the current scan to finish first."));
        return;
    }

    LibraryFoldersDialog dialog(m_app, this);
    connect(&dialog, &LibraryFoldersDialog::librariesChanged, this, [this] {
        m_app.libraryWatcher().refresh();
        m_model->reload();
        m_taxonModel->reload();
        m_bestShotModel->reload();
        m_reviewPane->reload();
        updateEmptyState();
        refreshCoverage();
        updateReviewTabText();
        updateBestShotsTabText();
        statusBar()->showMessage(tr("Library folders updated."), 6000);
    });
    dialog.exec();
}

void MainWindow::startScan()
{
    if (!ensureCanEdit())
        return;
    const QStringList roots = m_app.settings().watchedRoots();
    if (roots.isEmpty()) {
        QMessageBox::information(this, tr("Rescan Library"),
                                tr("Add a folder to the library first."));
        return;
    }
    if (m_app.scanService().isRunning())
        return;

    m_app.scanService().start(roots);
    beginScanProgress(tr("Rescanning the Library"));
}

void MainWindow::beginScanProgress(const QString &title)
{
    if (m_taskProgress)
        return;   // another task's window is up; the status bar still reports
    m_scanProgressActive = true;
    beginTaskProgress(title, [this] {
        m_app.scanService().cancel();
        updateTaskProgress(tr("Stopping…"), 0, 0);
    });
    updateTaskProgress(tr("Looking for photos…"), 0, 0);
}

void MainWindow::requestScan(std::function<void()> afterScan)
{
    if (!m_app.catalogueEditable())
        return;   // e.g. the folder watcher, on a read-only shared copy
    if (afterScan)
        m_afterQueuedScan.append(std::move(afterScan));
    if (m_app.scanService().isRunning()) {
        m_rescanQueued = true;   // picked up by onScanFinished()
        return;
    }
    runQueuedScan();
}

void MainWindow::runQueuedScan()
{
    const QList<std::function<void()>> callbacks = std::exchange(m_afterQueuedScan, {});
    if (!callbacks.isEmpty()) {
        connect(&m_app.scanService(), &scan::ScanService::finished, this,
                [callbacks](const scan::ScanSummary &) {
                    for (const auto &callback : callbacks)
                        callback();
                },
                Qt::SingleShotConnection);
    }
    startScan();
}

void MainWindow::setScanUiRunning(bool running)
{
    m_addFolderAction->setEnabled(!running);
    m_manageFoldersAction->setEnabled(!running);
    m_scanAction->setEnabled(!running);
    m_cancelAction->setEnabled(running);
    if (running)
        statusBar()->showMessage(tr("Scanning…"));
}

void MainWindow::onScanProgress(const scan::ScanProgress &progress)
{
    if (m_inatPipelineActive && !m_inatPipelineCancelled)
        updateTaskProgress(tr("Adding the downloaded photos to your library — scanning "
                              "(%1 photos checked)…")
                               .arg(progress.filesSeen),
                           0, 0);
    if (m_matchProgressActive && !m_matchCancelled)
        updateTaskProgress(tr("Checking the library for changes — %1 photos checked…")
                               .arg(progress.filesSeen),
                           0, 0);
    // Finding files first, then writing them into the catalogue.
    if (m_scanProgressActive)
        updateTaskProgress(progress.capturesSeen > 0
                               ? tr("Adding photos to the catalogue — %L1 processed…")
                                     .arg(progress.capturesSeen)
                               : tr("Looking for photos — %L1 folders, %L2 photos found…")
                                     .arg(progress.foldersSeen)
                                     .arg(progress.filesSeen),
                           0, 0);
    // foldersSeen/filesSeen are running totals for this whole scan (every
    // root being scanned this run, not just currentDir) -- word it that way
    // so it doesn't read as "this folder alone has thousands of photos" when
    // a scan covering several roots happens to be passing through a small one.
    statusBar()->showMessage(tr("Scanning — %1 folders, %2 photos found so far (currently: %3)")
                                 .arg(progress.foldersSeen)
                                 .arg(progress.filesSeen)
                                 .arg(progress.currentDir));
}

void MainWindow::onScanFinished(const scan::ScanSummary &summary)
{
    setScanUiRunning(false);
    if (std::exchange(m_scanProgressActive, false))
        endTaskProgress();
    scheduleChangesCount();
    m_lastScanCompleted = summary.ok() && !summary.cancelled;
    m_model->reload();
    updateEmptyState();
    m_app.libraryWatcher().refresh();

    if (!summary.ok()) {
        statusBar()->showMessage(tr("Scan failed: %1").arg(summary.error), 10000);
    } else if (summary.cancelled) {
        statusBar()->showMessage(tr("Scan stopped."), 5000);
    } else {
        statusBar()->showMessage(
            tr("Scan complete — %1 new, %2 updated, %3 unchanged")
                .arg(summary.capturesAdded)
                .arg(summary.capturesUpdated + summary.renditionsUpdated)
                .arg(summary.renditionsUnchanged),
            8000);
    }

    if (m_rescanQueued) {
        m_rescanQueued = false;
        // Deferred: the scan service only reports itself idle once this
        // finished signal has been fully delivered.
        QTimer::singleShot(0, this, &MainWindow::runQueuedScan);
    }
}

void MainWindow::restoreLayout()
{
    const QByteArray geometry = m_app.settings().mainWindowGeometry();
    if (geometry.isEmpty())
        resize(1100, 760);
    else
        restoreGeometry(geometry);

    const QByteArray state = m_app.settings().mainWindowState();
    if (!state.isEmpty())
        restoreState(state);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Editing a shared catalogue: offer to send the changes and hand the
    // edit lock back before going.
    if (m_app.settings().syncEnabled() && m_app.settings().syncEditing() && !m_closeConfirmed) {
        if (m_closeAfterSync) {   // already finishing; wait for it
            event->ignore();
            return;
        }
        const int pending = m_app.syncService().pendingChanges(m_app.database().connectionName());
        QMessageBox box(QMessageBox::Question, tr("Finish Editing?"),
                        pending > 0 ? tr("You have %n change(s) not yet sent to the shared "
                                         "catalogue.", nullptr, pending)
                                    : tr("You're still editing the shared catalogue, so nobody "
                                         "else can edit it."),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Finish Editing sends your changes and lets others edit."));
        QPushButton *finish = box.addButton(tr("Finish Editing"), QMessageBox::AcceptRole);
        QPushButton *keep = box.addButton(tr("Keep Editing Later"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(finish);
        box.exec();
        if (box.clickedButton() == finish) {
            m_closeAfterSync = true;
            runSync(sync::SyncService::Op::FinishEditing, true);
            event->ignore();
            return;
        }
        if (box.clickedButton() != keep) {
            event->ignore();
            return;
        }
    }
    m_app.scanService().cancel();
    m_app.settings().setMainWindowGeometry(saveGeometry());
    m_app.settings().setMainWindowState(saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::showHelp()
{
    if (!m_helpWindow)
        m_helpWindow = new HelpWindow(this);
    m_helpWindow->showPage();
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        tr("About %1").arg(QString::fromLatin1(kAppName)),
        tr("<h3>%1 %2</h3><p>Catalogue nature photos against a taxonomic tree.</p>"
           "<p style='color:gray'>Build %3 · Qt %4</p>")
            .arg(QString::fromLatin1(kAppName), QString::fromLatin1(kAppVersion),
                 QString::fromLatin1(kAppVersionFull), QString::fromLatin1(qVersion())));
}

void MainWindow::showStorageUsage()
{
    // A shared Postgres catalogue has no local file to size -- only local
    // SQLite mode has a byte count worth reporting here.
    const bool isLocalSqlite = m_app.settings().catalogueDescriptor().backend
                                == CatalogueDescriptor::Backend::Sqlite;
    qint64 dbBytes = 0;
    if (isLocalSqlite) {
        const QString dbPath = m_app.settings().databasePath();
        dbBytes = QFileInfo(dbPath).size();
        dbBytes += QFileInfo(dbPath + QStringLiteral("-wal")).size();
        dbBytes += QFileInfo(dbPath + QStringLiteral("-shm")).size();
    }

    const qint64 thumbBytes = app::directoryBytes(m_app.thumbnails().cacheDir());
    const qint64 photoBytes = app::directoryBytes(m_app.photoCache().cacheDir());
    const qint64 tileBytes = app::directoryBytes(m_app.tileCache().cacheDir());
    const qint64 total = dbBytes + thumbBytes + photoBytes + tileBytes;

    QLocale locale;
    const QString dbSizeText =
        isLocalSqlite ? locale.formattedDataSize(dbBytes) : tr("Shared (Postgres)");
    const QString html =
        tr("<table cellspacing='6'>"
           "<tr><td>Catalogue database</td><td align='right'>%1</td></tr>"
           "<tr><td>Photo thumbnails</td><td align='right'>%2</td></tr>"
           "<tr><td>Reference photos (iNaturalist)</td><td align='right'>%3</td></tr>"
           "<tr><td>Map tiles (OpenStreetMap)</td><td align='right'>%4</td></tr>"
           "<tr><td><b>Total</b></td><td align='right'><b>%5</b></td></tr>"
           "</table>")
            .arg(dbSizeText, locale.formattedDataSize(thumbBytes),
                 locale.formattedDataSize(photoBytes), locale.formattedDataSize(tileBytes),
                 locale.formattedDataSize(total));

    QMessageBox::information(this, tr("Storage Usage"), html);
}

void MainWindow::showCatalogueSettings()
{
    ManageCataloguesDialog dialog(m_app.settings(), this);
    connect(&dialog, &ManageCataloguesDialog::activeServerEdited, this,
            &MainWindow::applyServerSettings);
    connect(&dialog, &ManageCataloguesDialog::openRequested, this, [this, &dialog](const QString &id) {
        dialog.accept();
        // After the dialog has closed.
        QTimer::singleShot(0, this, [this, id] { switchToCatalogue(id); });
    });
    dialog.exec();
}

void MainWindow::editServerSettings()
{
    auto &settings = m_app.settings();
    if (!settings.syncEnabled())
        return;
    const Settings::CatalogueProfile before = settings.activeProfile();
    CatalogueSettingsDialog dialog(before, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Settings::CatalogueProfile after = dialog.profile();
    if (settings.syncEditing()
        && (after.pg.pgHost != before.pg.pgHost || after.pg.pgDbName != before.pg.pgDbName)) {
        QMessageBox::warning(this, tr("Server Settings"),
                             tr("Finish Editing before pointing this catalogue at another server."));
        return;
    }
    settings.saveServerProfile(after);
    applyServerSettings(before);
}

// After the open server catalogue's settings were saved -- from Server
// Settings or Manage Catalogues: use them now, and offer the first photo sync.
void MainWindow::applyServerSettings(const Settings::CatalogueProfile &before)
{
    auto &settings = m_app.settings();
    const Settings::CatalogueProfile after = settings.activeProfile();
    m_app.configureSync();
    const bool photoStoreChanged =
        after.syncsPhotos()
        && (!before.syncsPhotos() || after.photoStore != before.photoStore
            || after.s3.endpoint != before.s3.endpoint || after.s3.bucket != before.s3.bucket
            || after.s3.prefix != before.s3.prefix || after.libraryRoot != before.libraryRoot
            || after.drive.folderId != before.drive.folderId
            || after.drive.refreshToken != before.drive.refreshToken);
    if (photoStoreChanged) {
        m_serverView.photosPlan.reset();
        m_serverView.photosError.clear();
    }
    updatePhotoStatus();
    const CatalogueDescriptor server = settings.syncServerDescriptor();
    m_serverView.server = tr("%1 on %2").arg(server.pgDbName, server.pgHost);
    m_serverView.me = m_app.syncService().config().identity;
    updateSyncStatus();
    updateCatalogueCorner();
    statusBar()->showMessage(tr("Server settings saved."), 6000);
    if (photoStoreChanged
        && QMessageBox::question(
               this, tr("Sync Photos"),
               tr("Sync the photos with the photo storage now?\n\nThe first sync checks every "
                  "photo in the library folder, so it can take a while for a big library. "
                  "Photos already in both places aren't transferred again."))
               == QMessageBox::Yes)
        runPhotoSync(libsync::PhotoSyncService::Mode::Both, true);
}

void MainWindow::buildCatalogueCorner()
{
    m_catalogueCorner = new QWidget(this);
    auto *layout = new QHBoxLayout(m_catalogueCorner);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_catalogueSwitcher = new QToolButton(m_catalogueCorner);
    m_catalogueSwitcher->setAutoRaise(true);
    m_catalogueSwitcher->setPopupMode(QToolButton::InstantPopup);
    m_catalogueSwitcher->setToolTip(tr("Which catalogue is open. Click to switch catalogue."));
    auto *menu = new QMenu(m_catalogueSwitcher);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        const QString active = m_app.settings().activeProfile().id;
        for (const Settings::CatalogueProfile &p : m_app.settings().profiles()) {
            QAction *a = menu->addAction(p.server ? tr("Server: %1").arg(p.name) : p.name);
            a->setCheckable(true);
            a->setChecked(p.id == active);
            connect(a, &QAction::triggered, this, [this, id = p.id] { switchToCatalogue(id); });
        }
        menu->addSeparator();
        menu->addAction(tr("Manage Catalogues…"), this, &MainWindow::showCatalogueSettings);
    });
    m_catalogueSwitcher->setMenu(menu);
    layout->addWidget(m_catalogueSwitcher);

    if (!m_app.settings().syncEnabled()) {
        m_localBadge = new QLabel(m_catalogueCorner);
        m_localBadge->setToolTip(tr("Your own catalogue on this computer — not shared, never "
                                    "synced."));
        layout->addWidget(m_localBadge);
        setWindowTitle(tr("%1 — Local catalogue").arg(QString::fromLatin1(kAppName)));
    }
    menuBar()->setCornerWidget(m_catalogueCorner, Qt::TopRightCorner);
    updateCatalogueCorner();
}

void MainWindow::updateCatalogueCorner()
{
    if (!m_catalogueCorner)
        return;
    const Settings::CatalogueProfile p = m_app.settings().activeProfile();
    m_catalogueSwitcher->setText(p.server ? tr("Server: %1").arg(p.name) : tr("Local catalogue"));
    if (m_localBadge) {
        const QColor c = themeColors(currentThemeVariant()).secondaryAccent;
        m_localBadge->setText(QStringLiteral("⬤  ") + tr("LOCAL CATALOGUE"));
        m_localBadge->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; font-weight: 600; border: 1px solid %1; border-radius: 9px; "
            "padding: 1px 10px; margin-right: 6px; }")
                                        .arg(c.name()));
    }
    m_catalogueCorner->adjustSize();
}

void MainWindow::switchToCatalogue(const QString &id)
{
    auto &settings = m_app.settings();
    if (id == settings.activeProfile().id || !settings.profile(id))
        return;
    // Leaving a server catalogue you're editing: offer to finish first.
    if (settings.syncEnabled() && settings.syncEditing()) {
        const int pending = m_app.syncService().pendingChanges(m_app.database().connectionName());
        QMessageBox box(QMessageBox::Question, tr("Switch Catalogue"),
                        tr("You're editing \"%1\".").arg(settings.activeProfile().name),
                        QMessageBox::NoButton, this);
        box.setInformativeText(
            (pending > 0 ? tr("%n change(s) haven't been sent yet. ", nullptr, pending) : QString())
            + tr("Finish Editing sends them and lets others edit; or switch now and carry on "
                 "editing when you come back (others stay read-only meanwhile)."));
        QPushButton *finish = box.addButton(tr("Finish Editing, Then Switch"), QMessageBox::AcceptRole);
        QPushButton *keep = box.addButton(tr("Switch, Keep Editing Later"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(finish);
        box.exec();
        if (box.clickedButton() == finish) {
            m_switchAfterSync = id;
            runSync(sync::SyncService::Op::FinishEditing, true);
            return;
        }
        if (box.clickedButton() != keep)
            return;
    }
    settings.setActiveProfile(id);
    relaunch();
}

// --- shared catalogue (sync mode) ------------------------------------------

namespace {

using Op = sync::SyncService::Op;

QString describeLockTime(const QString &iso)
{
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid())
        t = QDateTime::fromString(iso, Qt::ISODate);
    return t.isValid() ? describeTime(t) : QString();
}

QString syncOpTitle(Op op)
{
    switch (op) {
    case Op::Check:
        return QObject::tr("Checking the shared catalogue");
    case Op::Pull:
        return QObject::tr("Getting the latest catalogue");
    case Op::StartEditing:
        return QObject::tr("Starting to edit");
    case Op::BreakLock:
        return QObject::tr("Taking over editing");
    case Op::Push:
        return QObject::tr("Sending your changes");
    case Op::FinishEditing:
        return QObject::tr("Finishing editing");
    case Op::Publish:
    case Op::PublishFromFile:
        return QObject::tr("Publishing the catalogue");
    case Op::Changes:
        return QObject::tr("Counting your changes");
    case Op::TestConnection:
        return QObject::tr("Testing the connection");
    case Op::Discard:
    case Op::DiscardAndRelease:
        return QObject::tr("Discarding your changes");
    case Op::RestoreBackup:
        return QObject::tr("Restoring a backup");
    }
    return {};
}

// The activity log's name for an operation.
QString syncOpName(Op op)
{
    switch (op) {
    case Op::Check:
        return QObject::tr("Check server");
    case Op::Pull:
        return QObject::tr("Get latest");
    case Op::StartEditing:
        return QObject::tr("Start editing");
    case Op::BreakLock:
        return QObject::tr("Take over editing");
    case Op::Push:
        return QObject::tr("Sync");
    case Op::FinishEditing:
        return QObject::tr("Finish editing");
    case Op::Publish:
        return QObject::tr("Publish");
    case Op::PublishFromFile:
        return QObject::tr("Publish a copy of the Local catalogue");
    case Op::Changes:
        return QObject::tr("Count changes");
    case Op::TestConnection:
        return QObject::tr("Test connection");
    case Op::Discard:
        return QObject::tr("Discard changes");
    case Op::DiscardAndRelease:
        return QObject::tr("Discard changes and stop editing");
    case Op::RestoreBackup:
        return QObject::tr("Restore backup");
    }
    return {};
}

bool isLocalOp(Op op)
{
    return op == Op::Changes || op == Op::Discard || op == Op::RestoreBackup;
}

} // namespace

void MainWindow::buildServerMenu(QMenu *menu)
{
    menu->setToolTipsVisible(true);
    m_serverStatusAction = menu->addAction(tr("Server &Status…"), this, &MainWindow::showServerStatus);
    menu->addSeparator();
    m_startEditingAction = menu->addAction(tr("&Start Editing"), this, [this] { startEditing(); });
    m_syncNowAction = menu->addAction(tr("S&ync Now"), this, &MainWindow::syncNow);
    m_syncNowAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    m_syncPhotosAction = menu->addAction(tr("Sync &Photos"), this, [this] {
        runPhotoSync(libsync::PhotoSyncService::Mode::Both, true);
    });
    m_syncPhotosAction->setToolTip(tr("Upload the photos added or changed on this computer, and "
                                      "download the ones collaborators have added"));
    m_syncPhotosAction->setVisible(false);
    m_stopPhotosAction = menu->addAction(tr("S&top Photo Sync"), this, &MainWindow::stopPhotoSync);
    m_stopPhotosAction->setToolTip(tr("Stop uploading or downloading photos; the next sync "
                                      "carries on where this one stopped"));
    m_stopPhotosAction->setVisible(false);
    m_photosHereAction = menu->addAction(tr("Photos on This &Computer…"), this,
                                         &MainWindow::choosePhotosOnComputer);
    m_photosHereAction->setToolTip(tr("Keep every photo on this computer, or only those of some "
                                      "reference trees"));
    m_photosHereAction->setVisible(false);
    m_finishEditingAction = menu->addAction(tr("&Finish Editing (Send && Release)"), this,
                                            &MainWindow::finishEditing);
    m_discardAction = menu->addAction(tr("&Discard My Changes…"), this, &MainWindow::discardChanges);
    menu->addSeparator();
    m_reviewChangesAction = menu->addAction(tr("&Review Changes…"), this, &MainWindow::showReviewChanges);
    menu->addAction(tr("Activity &Log…"), this, &MainWindow::showActivityLog);
    m_testConnectionAction = menu->addAction(tr("&Test Connection"), this, &MainWindow::testConnection);
    menu->addSeparator();
    m_breakLockAction = menu->addAction(tr("Take &Over Editing…"), this, &MainWindow::offerBreakLock);
    m_restoreBackupAction = menu->addAction(tr("Restore a &Backup…"), this, &MainWindow::restoreBackup);
    menu->addSeparator();
    menu->addAction(tr("Server Se&ttings…"), this, &MainWindow::editServerSettings);
    menu->addAction(tr("Switch to &Local Catalogue"), this, &MainWindow::switchToLocalCatalogue);
    menu->addAction(tr("&Manage Catalogues…"), this, &MainWindow::showCatalogueSettings);
}

void MainWindow::setupSync()
{
    const bool enabled = m_app.settings().syncEnabled();
    m_serverMenu->menuAction()->setVisible(enabled);
    if (!enabled)
        return;

    auto &service = m_app.syncService();
    const CatalogueDescriptor server = m_app.settings().syncServerDescriptor();
    m_serverView.me = service.config().identity;
    m_serverView.server = tr("%1 on %2").arg(server.pgDbName, server.pgHost);
    m_serverView.libraryRoot = m_app.settings().libraryRoot();
    {
        QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
        if (q.exec(QStringLiteral("SELECT version FROM catalogue_sync WHERE id = 1")) && q.next())
            m_serverView.localVersion = q.value(0).toLongLong();
    }

    m_serverBar = new ServerBar(m_serverMenu, m_catalogueCorner);
    m_catalogueCorner->layout()->addWidget(m_serverBar);
    connect(m_serverBar, &ServerBar::statusRequested, this, &MainWindow::showServerStatus);
    connect(m_serverBar, &ServerBar::startEditingRequested, this, [this] { startEditing(); });
    connect(m_serverBar, &ServerBar::getLatestRequested, this, [this] { runSync(Op::Check, true); });
    connect(m_serverBar, &ServerBar::syncRequested, this, &MainWindow::syncNow);
    connect(m_serverBar, &ServerBar::finishEditingRequested, this, &MainWindow::finishEditing);
    connect(m_serverBar, &ServerBar::reviewChangesRequested, this, &MainWindow::showReviewChanges);
    connect(m_serverBar, &ServerBar::takeOverRequested, this, &MainWindow::offerBreakLock);
    connect(m_serverBar, &ServerBar::retryRequested, this, [this] { runSync(Op::Check, true); });

    connect(&service, &sync::SyncService::progress, this,
            [this](const QString &phase, int done, int total) {
                m_serverView.runningPhase = phase;
                if (m_serverBar)
                    m_serverBar->setView(m_serverView);
                if (m_syncUserInitiated)
                    updateTaskProgress(phase, done, total);
            });
    connect(&service, &sync::SyncService::finished, this, &MainWindow::onSyncFinished);

    // Keep an eye on the server: who's editing, whether there's a newer
    // version, whether it's reachable.
    m_syncCheckTimer = new QTimer(this);
    connect(m_syncCheckTimer, &QTimer::timeout, this, [this] {
        if (!m_app.syncService().isRunning())
            runSync(Op::Check, false);
    });
    m_syncCheckTimer->start(m_app.settings().syncEditing() ? 2 * 60 * 1000 : 5 * 60 * 1000);
    // ...and recount your changes a moment after edits (local, quick).
    m_changesTimer = new QTimer(this);
    m_changesTimer->setSingleShot(true);
    m_changesTimer->setInterval(2000);
    connect(m_changesTimer, &QTimer::timeout, this, [this] {
        if (m_app.syncService().isRunning())
            m_changesTimer->start();   // try again shortly
        else
            runSync(Op::Changes, false);
    });
    // A server catalogue opened for the first time has an empty working copy:
    // offer to download (or start) it. Otherwise just look at the server.
    if (!QFile::exists(sync::SyncService::basePath(m_app.settings().databasePath())))
        QTimer::singleShot(0, this, &MainWindow::setUpSharedCatalogue);
    else
        QTimer::singleShot(0, this, [this] { runSync(Op::Check, false); });

    connect(m_reviewPane, &ReviewPane::editRequested, this, [this] { ensureCanEdit(); });
    setupPhotoSync();
    applyEditability();
}

void MainWindow::scheduleChangesCount()
{
    if (m_changesTimer && m_app.settings().syncEditing())
        m_changesTimer->start();
}

void MainWindow::runSync(Op op, bool userInitiated)
{
    if (m_app.syncService().isRunning()) {
        // A quick change count is under way: do this straight after it.
        if (userInitiated && m_runningOp == Op::Changes) {
            m_queuedOp = op;
            return;
        }
        if (userInitiated)
            statusBar()->showMessage(tr("The server is busy with another request — try again in "
                                        "a moment."),
                                     6000);
        return;
    }
    // Photos before the catalogue that refers to them: collaborators look for
    // them as soon as they get it.
    if ((op == Op::Push || op == Op::FinishEditing) && m_app.photoSync().config().enabled()
        && !std::exchange(m_photosUploadedForOp, false)) {
        m_opAfterPhotoUpload = op;
        m_opAfterPhotoUploadUser = userInitiated;
        if (!m_app.photoSync().isRunning())
            runPhotoSync(libsync::PhotoSyncService::Mode::Upload, userInitiated);
        else if (userInitiated)
            statusBar()->showMessage(tr("Waiting for the photo sync under way to finish…"), 6000);
        return;
    }
    m_runningOp = op;
    m_syncUserInitiated = userInitiated;
    if (userInitiated && op != Op::Changes)
        beginTaskProgress(syncOpTitle(op), {});
    if (op != Op::Changes) {
        m_serverView.running = true;
        m_serverView.runningPhase = syncOpTitle(op);
    }
    m_app.syncService().run(op);
    updateSyncStatus();
}

void MainWindow::onSyncFinished(const sync::SyncService::Outcome &o)
{
    if (m_syncSetupRunning)
        return;
    const bool userInitiated = std::exchange(m_syncUserInitiated, false);
    if (userInitiated && o.op != Op::Changes)
        endTaskProgress();
    m_runningOp.reset();
    // Get Latest gets the latest photos too.
    if (o.op == Op::Check && userInitiated && o.ok)
        runPhotoSync(libsync::PhotoSyncService::Mode::Download, false);
    auto &settings = m_app.settings();
    const sync::SyncIdentity me = m_app.syncService().config().identity;
    ServerView &v = m_serverView;
    const ServerView before = v;

    v.running = false;
    v.runningPhase.clear();
    if (o.state)
        v.state = o.state;
    if (!isLocalOp(o.op) && (o.ok || o.localVersion > 0))
        v.localVersion = o.localVersion;
    if (o.changes)
        v.changes = o.changes;
    else if (o.ok && (o.op == Op::Push || o.op == Op::FinishEditing || o.op == Op::Publish))
        v.changes = sync::ChangeSummary{};   // all sent
    if (o.test)
        v.lastTest = o.test;
    if (!isLocalOp(o.op)) {
        if (o.ok || !o.unreachable) {
            v.offline = false;
            v.checked = v.checked || o.ok;
            v.lastContact = o.finishedAt;
        } else {
            v.offline = true;
            v.lastError = o.error;
        }
    }
    if (o.ok && (o.op == Op::Push || o.op == Op::FinishEditing || o.op == Op::Publish
                 || o.op == Op::Pull))
        v.lastSync = o.finishedAt;

    // The activity log: the operation, and anything that changed on its own.
    if (o.op != Op::Changes && (o.op != Op::Check || userInitiated || !o.ok))
        logActivity(o);
    sync::SyncActivityLog log(QDir(QFileInfo(settings.databasePath()).absolutePath())
                                  .filePath(QStringLiteral("sync-activity.log")));
    const QDateTime now = QDateTime::currentDateTime();
    if (before.offline != v.offline && !(o.op == Op::Check && !o.ok && !before.checked))
        log.append({now, v.offline ? tr("Connection lost") : tr("Connection restored"), !v.offline,
                    v.offline ? QString() : v.server, v.offline ? v.lastError : QString(), 0});
    const QString lockBefore = before.state ? before.state->lockedBy : QString();
    const QString lockNow = v.state ? v.state->lockedBy : QString();
    if (before.state && v.state && lockBefore != lockNow && !(v.state->heldBy(me)))
        log.append({now, tr("Edit lock"), true,
                    lockNow.isEmpty() ? tr("%1 finished editing").arg(lockBefore)
                                      : tr("%1 started editing").arg(lockNow),
                    {}, 0});
    if (before.state && v.state && v.state->version > before.state->version
        && v.state->pushedBy != me.user)
        log.append({now, tr("Newer version on server"), true,
                    tr("Version %1 by %2").arg(v.state->version).arg(v.state->pushedBy), {}, 0});

    if (!o.ok) {
        if (o.op == Op::FinishEditing && !m_switchAfterSync.isEmpty()) {
            m_switchAfterSync.clear();
            QMessageBox::warning(this, tr("Switch Catalogue"),
                                 tr("Your changes couldn't be sent, so you're still in this "
                                    "catalogue:\n\n%1")
                                     .arg(o.error));
            updateSyncStatus();
            return;
        }
        if (m_closeAfterSync) {
            m_closeAfterSync = false;
            const auto reply = QMessageBox::question(
                this, tr("Finish Editing"),
                tr("Your changes couldn't be sent:\n\n%1\n\nClose anyway? They stay on this "
                   "computer, and you keep the edit lock.")
                    .arg(o.error));
            if (reply == QMessageBox::Yes) {
                m_closeConfirmed = true;
                close();
            }
        } else if ((o.op == Op::StartEditing || o.op == Op::BreakLock) && o.state
                   && o.state->isLocked() && !o.state->heldBy(me)) {
            offerBreakLock();
        } else if (userInitiated) {
            QMessageBox::warning(this, syncOpTitle(o.op), o.error);
        } else if (o.op != Op::Changes) {
            qInfo().noquote() << "Shared catalogue check failed:" << o.error;
        }
        updateSyncStatus();
        if (m_queuedOp)
            runSync(*std::exchange(m_queuedOp, std::nullopt), true);
        return;
    }

    // Someone broke our lock: we're no longer editing.
    if (settings.syncEditing() && o.state && !o.state->heldBy(me) && o.op != Op::FinishEditing
        && o.op != Op::DiscardAndRelease) {
        settings.setSyncEditing(false);
        applyEditability();
        QMessageBox::warning(
            this, tr("Editing Taken Over"),
            tr("%1 has taken over editing the shared catalogue, so this computer is read-only "
               "now. Changes you hadn't sent can't be sent any more; use Get Latest to update.")
                .arg(o.state->isLocked() ? o.state->lockedBy : tr("Someone")));
    }

    switch (o.op) {
    case Op::Check:
        if (userInitiated) {
            if (!v.newerAvailable()) {
                statusBar()->showMessage(tr("Your catalogue is up to date."), 6000);
                break;
            }
            QString text = tr("A newer shared catalogue is available (version %1%2). Get it now? "
                              "PhotoLife restarts to open it; a backup of your current catalogue "
                              "is kept.")
                               .arg(o.state->version)
                               .arg(o.state->pushedBy.isEmpty()
                                        ? QString()
                                        : tr(", by %1").arg(o.state->pushedBy));
            if (v.pending() > 0)
                text += QStringLiteral("\n\n")
                        + tr("%n change(s) made on this computer while not editing will be "
                             "replaced.",
                             nullptr, v.pending());
            if (QMessageBox::question(this, tr("Get Latest"), text) == QMessageBox::Yes)
                runSync(Op::Pull, true);
        }
        break;
    case Op::Pull:
        QMessageBox::information(this, tr("Get Latest"),
                                 tr("Got the latest shared catalogue. PhotoLife will restart to "
                                    "open it."));
        relaunch();
        return;
    case Op::StartEditing:
    case Op::BreakLock:
        settings.setSyncEditing(true);
        if (m_syncCheckTimer)
            m_syncCheckTimer->setInterval(2 * 60 * 1000);
        if (o.pulled) {
            QMessageBox::information(this, tr("Start Editing"),
                                     tr("You're editing the shared catalogue. PhotoLife will "
                                        "restart to open its latest version first."));
            relaunch();
            return;
        }
        applyEditability();
        statusBar()->showMessage(tr("You're editing the shared catalogue — others see it "
                                    "read-only until you Finish Editing."),
                                 8000);
        break;
    case Op::Push:
    case Op::FinishEditing:
    case Op::Publish: {
        if (o.op == Op::FinishEditing) {
            settings.setSyncEditing(false);
            if (m_syncCheckTimer)
                m_syncCheckTimer->setInterval(5 * 60 * 1000);
            applyEditability();
        }
        QString message = o.sent > 0 ? tr("Sent %n change(s) to the shared catalogue.", nullptr, o.sent)
                                     : tr("No changes to send.");
        if (o.op == Op::FinishEditing)
            message += QLatin1Char(' ') + tr("Others can edit now.");
        statusBar()->showMessage(message, 8000);
        if (o.outsideRoot > 0)
            QMessageBox::warning(
                this, tr("Photos Outside Your Library"),
                tr("%n photo location(s) are outside your photo library folder (%1), so "
                   "collaborators won't be able to open those photos. Move them into the "
                   "library, or change the library folder in Server Settings.",
                   nullptr, o.outsideRoot)
                    .arg(QDir::toNativeSeparators(settings.libraryRoot())));
        if (m_closeAfterSync) {
            m_closeAfterSync = false;
            m_closeConfirmed = true;
            close();
            return;
        }
        if (o.op == Op::FinishEditing && !m_switchAfterSync.isEmpty()) {
            settings.setActiveProfile(std::exchange(m_switchAfterSync, QString()));
            relaunch();
            return;
        }
        break;
    }
    case Op::Changes:
        if (m_reviewDialog)
            m_reviewDialog->setSummary(v.changes, settings.syncEditing());
        break;
    case Op::TestConnection: {
        const auto &t = *o.test;
        QMessageBox::information(
            this, tr("Test Connection"),
            tr("Connected to %1.\n\nConnecting took %2 ms; a round trip takes %3 ms.\n\n"
               "Server: %4\nCatalogue version %5, schema %6.\nEdit lock: %7.")
                .arg(v.server)
                .arg(t.connectMs)
                .arg(QString::number(t.roundTripMs, 'f', 0))
                .arg(t.serverVersion.section(QLatin1Char(','), 0, 0))
                .arg(o.state->version)
                .arg(o.state->schemaVersion)
                .arg(o.state->isLocked() ? tr("%1 on %2 since %3")
                                               .arg(o.state->lockedBy, o.state->lockedMachine,
                                                    describeLockTime(o.state->lockedAt))
                                         : tr("free")));
        break;
    }
    case Op::Discard:
    case Op::DiscardAndRelease:
        if (o.op == Op::DiscardAndRelease)
            settings.setSyncEditing(false);
        QMessageBox::information(this, tr("Discard My Changes"),
                                 tr("PhotoLife will restart with the catalogue as it was at your "
                                    "last sync. The discarded version is kept as a backup."));
        relaunch();
        return;
    case Op::PublishFromFile:
        settings.setSyncEditing(true);
        relaunch();
        return;
    case Op::RestoreBackup:
        QMessageBox::information(this, tr("Restore a Backup"),
                                 tr("PhotoLife will restart with the restored catalogue. The one "
                                    "it replaces is kept as a backup."));
        relaunch();
        return;
    }
    if (!isLocalOp(o.op) && o.op != Op::Check)
        scheduleChangesCount();
    updateSyncStatus();
    if (m_queuedOp)
        runSync(*std::exchange(m_queuedOp, std::nullopt), true);
}

void MainWindow::logActivity(const sync::SyncService::Outcome &o)
{
    QString detail;
    if (o.ok) {
        switch (o.op) {
        case Op::Push:
        case Op::FinishEditing:
            detail = o.sent > 0 ? tr("Sent %n change(s); server now at version %1", nullptr, o.sent)
                                      .arg(o.localVersion)
                                : tr("Nothing to send");
            if (o.op == Op::FinishEditing)
                detail += tr("; edit lock released");
            break;
        case Op::Publish:
        case Op::PublishFromFile:
            detail = tr("Published %L1 rows (version %2)").arg(o.sent).arg(o.localVersion);
            break;
        case Op::Pull:
            detail = tr("Got version %1").arg(o.state ? o.state->version : 0);
            break;
        case Op::StartEditing:
        case Op::BreakLock:
            detail = o.pulled ? tr("Editing; got version %1 first").arg(o.state ? o.state->version : 0)
                              : tr("Editing");
            break;
        case Op::TestConnection:
            detail = tr("Connect %1 ms, round trip %2 ms")
                         .arg(o.test->connectMs)
                         .arg(QString::number(o.test->roundTripMs, 'f', 0));
            break;
        case Op::Check:
            detail = o.state ? tr("Server at version %1").arg(o.state->version) : QString();
            break;
        case Op::Discard:
        case Op::DiscardAndRelease:
            detail = tr("Went back to the last synced copy");
            break;
        case Op::RestoreBackup:
            detail = tr("Restored a backup");
            break;
        case Op::Changes:
            break;
        }
    }
    sync::SyncActivityLog(QDir(QFileInfo(m_app.settings().databasePath()).absolutePath())
                              .filePath(QStringLiteral("sync-activity.log")))
        .append({o.finishedAt.isValid() ? o.finishedAt : QDateTime::currentDateTime(),
                 syncOpName(o.op), o.ok, detail, o.error, o.durationMs});
}

void MainWindow::setupPhotoSync()
{
    auto &photos = m_app.photoSync();
    m_photoStatusLabel = new QLabel(this);
    m_photoStatusLabel->setToolTip(tr("The photos PhotoLife keeps in sync with the photo storage "
                                      "(Server ▸ Sync Photos)"));
    statusBar()->addPermanentWidget(m_photoStatusLabel);
    connect(&photos, &libsync::PhotoSyncService::progress, this,
            [this](const QString &phase, int done, int total) {
                m_serverView.photosPhase =
                    total > 0 ? tr("%1 (%L2 of %L3)").arg(phase).arg(done).arg(total) : phase;
                if (m_photoTaskProgress)
                    updateTaskProgress(phase, done, total);
                updatePhotoStatus();
            });
    connect(&photos, &libsync::PhotoSyncService::finished, this, &MainWindow::onPhotoSyncFinished);
    const QStringList onlineOnly = libsync::PhotoSyncService::onlineOnlyPaths(photos.config());
    CaptureDelegate::setOnlineOnly(QSet<QString>(onlineOnly.cbegin(), onlineOnly.cend()));
    updatePhotoStatus();

    // Fetch what collaborators have added since last time, in the background.
    if (photos.config().enabled())
        QTimer::singleShot(3000, this,
                           [this] { runPhotoSync(libsync::PhotoSyncService::Mode::Download, false); });
}

void MainWindow::runPhotoSync(libsync::PhotoSyncService::Mode mode, bool userInitiated)
{
    using Mode = libsync::PhotoSyncService::Mode;
    auto &photos = m_app.photoSync();
    if (!photos.config().enabled())
        return;
    if (photos.isRunning()) {
        if (userInitiated)
            statusBar()->showMessage(tr("The photos are already syncing."), 6000);
        return;
    }
    m_photoSyncUserInitiated = userInitiated;
    if ((mode == Mode::Upload && m_opAfterPhotoUpload && userInitiated) || mode == Mode::FreeUp) {
        m_photoTaskProgress = true;
        beginTaskProgress(mode == Mode::FreeUp ? tr("Freeing Up Space") : tr("Uploading Photos"), [this] {
            m_app.photoSync().cancel();
            showTaskCancelling(tr("Stopping after the current photo…"));
        });
    }
    m_serverView.photosRunning = true;
    m_serverView.photosStopped = false;
    m_serverView.photosPhase.clear();
    photos.run(mode);
    updatePhotoStatus();
}

void MainWindow::onPhotoSyncFinished(const libsync::PhotoSyncService::Outcome &o)
{
    using Mode = libsync::PhotoSyncService::Mode;
    const bool userInitiated = std::exchange(m_photoSyncUserInitiated, false);
    if (std::exchange(m_photoTaskProgress, false))
        endTaskProgress();
    ServerView &v = m_serverView;
    v.photosRunning = false;
    v.photosPhase.clear();
    v.photosError = o.ok ? QString() : o.error;
    v.photosFailed = 0;

    QString detail;
    QString error = o.error;
    bool changedHere = false;
    if (o.result) {
        const libsync::LibrarySyncResult &r = *o.result;
        // What's left: whatever this sync didn't try, and whatever failed.
        libsync::LibraryPlan rest = r.plan;
        if (o.mode != Mode::Download)
            rest.toUpload.clear();
        if (o.mode != Mode::Upload) {
            rest.toDownload.clear();
            rest.conflicts.clear();
        }
        v.photosPlan = r.cancelled ? std::nullopt : std::optional(rest);
        v.photosStopped = r.cancelled;
        v.photosFailed = int(r.failures.size());
        if (!r.cancelled)
            v.photosLastSync = o.finishedAt;
        changedHere = r.downloaded + r.conflicts + r.trashed > 0;

        QStringList parts;
        if (r.uploaded > 0)
            parts << tr("uploaded %Ln", nullptr, r.uploaded);
        if (r.downloaded > 0)
            parts << tr("downloaded %Ln", nullptr, r.downloaded);
        if (r.conflicts > 0)
            parts << tr("%Ln changed in both places (both copies kept)", nullptr, r.conflicts);
        if (r.trashed > 0)
            parts << tr("%Ln deleted online (moved to %1)", nullptr, r.trashed)
                         .arg(QLatin1String(libsync::LibrarySync::kTrashDir));
        if (r.freed > 0)
            parts << tr("%Ln removed from this computer to free %1 (still online)", nullptr, r.freed)
                         .arg(QLocale().formattedDataSize(r.bytesFreed));
        if (!r.plan.removedHere.isEmpty())
            parts << tr("%Ln deleted here (kept online)", nullptr, int(r.plan.removedHere.size()));
        detail = parts.isEmpty() ? tr("Photos up to date") : tr("Photos: %1").arg(parts.join(QStringLiteral(", ")));
        if (r.cancelled)
            detail += tr(" — stopped");
        if (!r.failures.isEmpty())
            error = tr("%Ln photo(s) couldn't be synced: %1", nullptr, int(r.failures.size()))
                        .arg(r.failures.mid(0, 5).join(QStringLiteral("; ")));
    } else if (o.plan) {
        v.photosPlan = o.plan;
    }

    if (o.ok) {
        CaptureDelegate::setOnlineOnly(QSet<QString>(o.onlineOnly.cbegin(), o.onlineOnly.cend()));
        for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(m_grid),
                                        static_cast<QAbstractItemView *>(m_taxonGrid),
                                        static_cast<QAbstractItemView *>(m_bestShotGrid),
                                        static_cast<QAbstractItemView *>(m_untreedGrid)}) {
            if (view)
                view->viewport()->update();
        }
    }
    if (o.thumbnails && o.thumbnails->downloaded > 0)
        changedHere = true;   // thumbnails to show for photos only online

    if (o.mode != Mode::Check) {
        const QString action = o.mode == Mode::Upload     ? tr("Upload Photos")
                               : o.mode == Mode::Download ? tr("Download Photos")
                               : o.mode == Mode::FreeUp   ? tr("Free Up Space")
                               : o.mode == Mode::Fetch    ? tr("Download Photos (opened)")
                                                          : tr("Sync Photos");
        sync::SyncActivityLog(QDir(QFileInfo(m_app.settings().databasePath()).absolutePath())
                                  .filePath(QStringLiteral("sync-activity.log")))
            .append({o.finishedAt.isValid() ? o.finishedAt : QDateTime::currentDateTime(), action,
                     o.ok && v.photosFailed == 0, detail, error, o.durationMs});
    }

    if (changedHere) {
        m_model->reload();
        m_bestShotModel->reload();
        m_reviewPane->reload();
    }

    // A catalogue send waiting on its photos goes ahead once nothing else is
    // syncing them.
    auto resumeSend = [this] {
        if (m_opAfterPhotoUpload && !m_app.photoSync().isRunning())
            runPhotoSync(libsync::PhotoSyncService::Mode::Upload, m_opAfterPhotoUploadUser);
    };
    if (o.mode == Mode::Fetch) {
        if (o.result && !o.result->failures.isEmpty())
            QMessageBox::warning(this, tr("Download Photos"),
                                 tr("Some photos couldn't be downloaded:\n\n%1")
                                     .arg(o.result->failures.mid(0, 8).join(QLatin1Char('\n'))));
        updatePhotoStatus();
        if (const auto then = std::exchange(m_afterFetch, {}))
            then();
        resumeSend();
        return;
    }
    if (o.mode == Mode::Check && std::exchange(m_selectionFollowUp, false)) {
        updatePhotoStatus();
        if (!o.ok || !o.plan) {
            QMessageBox::warning(this, tr("Photos on This Computer"),
                                 tr("Couldn't check the photo storage: %1").arg(o.error));
            resumeSend();
            return;
        }
        const libsync::LibraryPlan &plan = *o.plan;
        if (!plan.freeable.isEmpty()
            && QMessageBox::question(
                   this, tr("Free Up Space"),
                   tr("%Ln photo file(s) on this computer (%1) aren't in the reference trees you "
                      "keep, and are safely online. Delete these copies from this computer to "
                      "free up the space?\n\nThey stay online, and download again if you open "
                      "them or tick their trees.",
                      nullptr, int(plan.freeable.size()))
                       .arg(QLocale().formattedDataSize(plan.freeableBytes)))
                   == QMessageBox::Yes) {
            m_downloadAfterFreeUp = !plan.toDownload.isEmpty();
            runPhotoSync(Mode::FreeUp, true);
            return;
        }
        if (!plan.toDownload.isEmpty())
            runPhotoSync(Mode::Download, true);
        resumeSend();
        return;
    }
    if (o.mode == Mode::FreeUp && std::exchange(m_downloadAfterFreeUp, false)) {
        statusBar()->showMessage(detail, 10000);
        runPhotoSync(Mode::Download, true);
        updatePhotoStatus();
        return;
    }

    if (m_opAfterPhotoUpload) {
        if (o.mode != Mode::Upload && o.mode != Mode::Both) {
            // A background sync was under way: now the upload the send waits on.
            runPhotoSync(Mode::Upload, m_opAfterPhotoUploadUser);
            updatePhotoStatus();
            return;
        }
        const Op op = *std::exchange(m_opAfterPhotoUpload, std::nullopt);
        const bool user = m_opAfterPhotoUploadUser;
        bool proceed = true;
        if (o.result && o.result->cancelled) {
            proceed = false;
        } else if (!o.ok || v.photosFailed > 0) {
            proceed = QMessageBox::question(
                          this, syncOpTitle(op),
                          tr("Some photos couldn't be uploaded, so collaborators can't get them "
                             "yet — PhotoLife tries again at the next sync.\n\n%1\n\nSend the "
                             "catalogue anyway?")
                              .arg(error))
                      == QMessageBox::Yes;
        }
        if (proceed) {
            m_photosUploadedForOp = true;
            runSync(op, user);
        } else {
            m_closeAfterSync = false;
            m_switchAfterSync.clear();
        }
    } else if (userInitiated) {
        if (!o.ok || v.photosFailed > 0)
            QMessageBox::warning(this, tr("Sync Photos"), detail.isEmpty() ? error : detail + QStringLiteral("\n\n") + error);
        else
            statusBar()->showMessage(detail, 10000);
    }
    updatePhotoStatus();
}

void MainWindow::updatePhotoStatus()
{
    ServerView &v = m_serverView;
    const auto &config = m_app.photoSync().config();
    v.photosEnabled = config.enabled();
    v.photosOnlineOnly = int(CaptureDelegate::onlineOnly().size());
    v.photosKept = config.allTrees ? tr("every photo")
                                   : tr("the photos of %Ln reference tree(s)", nullptr,
                                        int(config.trees.size()));
    if (m_syncPhotosAction) {
        m_syncPhotosAction->setVisible(v.photosEnabled);
        m_syncPhotosAction->setEnabled(!v.photosRunning);
    }
    if (m_stopPhotosAction) {
        m_stopPhotosAction->setVisible(v.photosEnabled);
        m_stopPhotosAction->setEnabled(v.photosRunning);
    }
    if (m_photosHereAction) {
        m_photosHereAction->setVisible(v.photosEnabled);
        m_photosHereAction->setEnabled(!v.photosRunning);
    }
    if (m_photoStatusLabel) {
        m_photoStatusLabel->setText(ServerBar::photosText(v));
        m_photoStatusLabel->setVisible(v.photosEnabled);
    }
    if (m_statusDialog)
        m_statusDialog->setView(v);
}

void MainWindow::stopPhotoSync()
{
    if (!m_app.photoSync().isRunning())
        return;
    m_app.photoSync().cancel();
    m_serverView.photosPhase = tr("stopping after the current photo…");
    updatePhotoStatus();
}

void MainWindow::choosePhotosOnComputer()
{
    auto &settings = m_app.settings();
    Settings::CatalogueProfile profile = settings.activeProfile();
    if (!profile.syncsPhotos())
        return;
    if (m_app.photoSync().isRunning()) {
        QMessageBox::information(this, tr("Photos on This Computer"),
                                 tr("Wait for the photo sync under way to finish first."));
        return;
    }
    PhotosOnComputerDialog dialog(
        libsync::PhotoSelection::treeStats(m_app.database().connectionName()),
        profile.photosAllTrees, profile.photoTrees, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    profile.photosAllTrees = dialog.allTrees();
    profile.photoTrees = dialog.tickedTrees();
    settings.saveServerProfile(profile);
    m_app.configureSync();
    updatePhotoStatus();
    // See what that means: photos to fetch, and copies here that can go.
    m_selectionFollowUp = true;
    runPhotoSync(libsync::PhotoSyncService::Mode::Check, true);
}

void MainWindow::fetchPhotos(const QStringList &paths, std::function<void()> then)
{
    auto &photos = m_app.photoSync();
    if (paths.isEmpty()) {
        if (then)
            then();
        return;
    }
    if (photos.isRunning()) {
        QMessageBox::information(this, tr("Download Photos"),
                                 tr("The photos are syncing at the moment — try again when "
                                    "that's finished."));
        return;
    }
    m_afterFetch = std::move(then);
    m_photoTaskProgress = true;
    beginTaskProgress(tr("Downloading Photos"), [this] {
        m_app.photoSync().cancel();
        showTaskCancelling(tr("Stopping after the current photo…"));
    });
    m_serverView.photosRunning = true;
    if (!photos.fetch(paths)) {
        m_photoTaskProgress = false;
        m_serverView.photosRunning = false;
        endTaskProgress();
        m_afterFetch = {};
    }
    updatePhotoStatus();
}

QStringList MainWindow::onlineOnlyRenditions(const QList<int> &captureIds) const
{
    QStringList out;
    if (captureIds.isEmpty() || CaptureDelegate::onlineOnly().isEmpty())
        return out;
    QStringList ids;
    for (const int id : captureIds)
        ids << QString::number(id);
    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    if (!q.exec(QStringLiteral("SELECT path FROM rendition WHERE capture_id IN (%1)")
                    .arg(ids.join(QLatin1Char(',')))))
        return out;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (CaptureDelegate::isOnlineOnly(path))
            out << path;
    }
    return out;
}

QStringList MainWindow::onlineOnlyRenditionsOfTree(int projectId) const
{
    QStringList out;
    if (CaptureDelegate::onlineOnly().isEmpty())
        return out;
    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT r.path FROM rendition r WHERE r.capture_id IN ("
        " SELECT m.capture_id FROM capture_match m"
        " JOIN project_taxon pt ON pt.taxon_id = m.taxon_id"
        " WHERE pt.project_id = ? AND m.status IN ('auto', 'confirmed'))"));
    q.addBindValue(projectId);
    if (!q.exec())
        return out;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (CaptureDelegate::isOnlineOnly(path))
            out << path;
    }
    return out;
}

bool MainWindow::confirmSend(bool finishing)
{
    if (!m_app.settings().syncConfirmSend())
        return true;
    QString error;
    const auto summary = sync::summarizeChanges(
        m_app.database().connectionName(),
        sync::SyncService::basePath(m_app.settings().databasePath()), &error);
    if (!summary || summary->isEmpty())
        return true;   // nothing to confirm; the sync says so
    m_serverView.changes = summary;
    updateSyncStatus();

    QMessageBox box(QMessageBox::Question, finishing ? tr("Finish Editing") : tr("Sync"),
                    tr("Send these changes to the shared catalogue?"), QMessageBox::NoButton, this);
    box.setInformativeText(summary->headline() + QStringLiteral("\n\n")
                           + (finishing ? tr("Others can edit once they're sent.")
                                        : tr("You keep editing afterwards.")));
    QPushButton *send = box.addButton(tr("Send"), QMessageBox::AcceptRole);
    QPushButton *review = box.addButton(tr("Review…"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(send);
    auto *dontAsk = new QCheckBox(tr("Don't ask again"), &box);
    box.setCheckBox(dontAsk);
    box.exec();
    if (box.clickedButton() == review) {
        showReviewChanges();
        return false;
    }
    if (box.clickedButton() != send)
        return false;
    if (dontAsk->isChecked())
        m_app.settings().setSyncConfirmSend(false);
    return true;
}

void MainWindow::syncNow()
{
    // Editing: send what's changed. Otherwise: see whether there's newer.
    if (m_app.settings().syncEditing()) {
        if (confirmSend(false))
            runSync(Op::Push, true);
    } else {
        runSync(Op::Check, true);
    }
}

void MainWindow::startEditing(bool breakLock)
{
    runSync(breakLock ? Op::BreakLock : Op::StartEditing, true);
}

void MainWindow::finishEditing()
{
    if (confirmSend(true))
        runSync(Op::FinishEditing, true);
}

void MainWindow::offerBreakLock()
{
    const QString who = m_serverView.state ? m_serverView.state->lockedBy : tr("Someone");
    const QString since = m_serverView.state ? describeLockTime(m_serverView.state->lockedAt) : QString();
    QMessageBox box(QMessageBox::Information, tr("Take Over Editing"),
                    since.isEmpty() ? tr("%1 is editing the shared catalogue.").arg(who)
                                    : tr("%1 has been editing the shared catalogue since %2.")
                                          .arg(who, since),
                    QMessageBox::NoButton, this);
    box.setInformativeText(
        tr("Only one person edits at a time. If %1 has finished but forgot to Finish Editing "
           "(or their computer is off), you can take over — any changes they haven't sent "
           "yet will then be lost to them.")
            .arg(who));
    QPushButton *take = box.addButton(tr("Take Over Editing…"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != take)
        return;
    if (QMessageBox::warning(this, tr("Take Over Editing"),
                             tr("Take over editing from %1?").arg(who),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        == QMessageBox::Yes)
        startEditing(true);
}

bool MainWindow::ensureCanEdit()
{
    if (m_app.catalogueEditable())
        return true;
    QString text = tr("The shared catalogue is read-only on this computer until you start "
                      "editing it.");
    if (m_serverView.lockedByOther())
        text += QLatin1Char(' ') + tr("%1 is editing it at the moment.").arg(m_serverView.state->lockedBy);
    if (QMessageBox::question(this, tr("Read-Only Catalogue"),
                              text + QStringLiteral("\n\n") + tr("Start editing now?"))
        == QMessageBox::Yes)
        startEditing();
    return false;
}

void MainWindow::applyEditability()
{
    const bool editable = m_app.catalogueEditable();
    m_reviewPane->setReadOnly(!editable);
    updateSyncStatus();
}

void MainWindow::updateSyncStatus()
{
    if (!m_serverBar)
        return;
    ServerView &v = m_serverView;
    v.editing = m_app.settings().syncEditing();
    v.backups = int(m_app.syncService().backups().size());
    v.libraryRoot = m_app.settings().libraryRoot();
    m_serverBar->setView(v);
    if (m_statusDialog)
        m_statusDialog->setView(v);

    const bool idle = !v.running;
    m_startEditingAction->setEnabled(idle && !v.editing && !v.offline);
    m_syncNowAction->setEnabled(idle && !(v.editing && v.offline));
    m_syncNowAction->setText(v.editing ? tr("S&ync Now (Send My Changes)")
                                       : tr("S&ync Now (Get Latest)"));
    m_finishEditingAction->setEnabled(idle && v.editing && !v.offline);
    m_discardAction->setEnabled(idle && v.pending() > 0);
    m_breakLockAction->setEnabled(idle && !v.editing && !v.offline && v.lockedByOther());
    m_testConnectionAction->setEnabled(idle);
    m_restoreBackupAction->setEnabled(idle && v.backups > 0);
    m_reviewChangesAction->setText(v.pending() > 0 ? tr("&Review Changes (%L1)…").arg(v.pending())
                                                   : tr("&Review Changes…"));

    setWindowTitle(QStringLiteral("%1 — %2").arg(QString::fromLatin1(kAppName),
                                                 v.editing ? tr("Editing shared catalogue")
                                                           : tr("Viewing shared catalogue")));
}

void MainWindow::showServerStatus()
{
    if (!m_statusDialog) {
        m_statusDialog = new ServerStatusDialog(this);
        m_statusDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_statusDialog, &ServerStatusDialog::startEditingRequested, this, [this] { startEditing(); });
        connect(m_statusDialog, &ServerStatusDialog::getLatestRequested, this,
                [this] { runSync(Op::Check, true); });
        connect(m_statusDialog, &ServerStatusDialog::syncRequested, this, &MainWindow::syncNow);
        connect(m_statusDialog, &ServerStatusDialog::finishEditingRequested, this, &MainWindow::finishEditing);
        connect(m_statusDialog, &ServerStatusDialog::reviewChangesRequested, this,
                &MainWindow::showReviewChanges);
        connect(m_statusDialog, &ServerStatusDialog::testConnectionRequested, this,
                &MainWindow::testConnection);
        connect(m_statusDialog, &ServerStatusDialog::activityLogRequested, this,
                &MainWindow::showActivityLog);
        connect(m_statusDialog, &ServerStatusDialog::restoreBackupRequested, this,
                &MainWindow::restoreBackup);
        connect(m_statusDialog, &ServerStatusDialog::syncPhotosRequested, this, [this] {
            if (m_app.photoSync().isRunning())
                stopPhotoSync();
            else
                runPhotoSync(libsync::PhotoSyncService::Mode::Both, true);
        });
    }
    m_statusDialog->setView(m_serverView);
    m_statusDialog->show();
    m_statusDialog->raise();
    m_statusDialog->activateWindow();
    runSync(Op::Check, false);   // freshen it up
}

void MainWindow::showReviewChanges()
{
    if (!m_reviewDialog) {
        m_reviewDialog = new ReviewChangesDialog(this);
        m_reviewDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_reviewDialog, &ReviewChangesDialog::refreshRequested, this,
                [this] { runSync(Op::Changes, false); });
        connect(m_reviewDialog, &ReviewChangesDialog::syncRequested, this, [this] {
            if (m_reviewDialog)
                m_reviewDialog->close();
            runSync(Op::Push, true);   // they've just reviewed: no second confirmation
        });
        connect(m_reviewDialog, &ReviewChangesDialog::discardRequested, this,
                &MainWindow::discardChanges);
    }
    m_reviewDialog->setSummary(std::nullopt, m_app.settings().syncEditing());
    m_reviewDialog->show();
    m_reviewDialog->raise();
    m_reviewDialog->activateWindow();
    runSync(Op::Changes, false);
}

void MainWindow::showActivityLog()
{
    if (!m_activityDialog) {
        m_activityDialog = new ActivityLogDialog(
            QDir(QFileInfo(m_app.settings().databasePath()).absolutePath())
                .filePath(QStringLiteral("sync-activity.log")),
            this);
        m_activityDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_activityDialog->reload();
    m_activityDialog->show();
    m_activityDialog->raise();
    m_activityDialog->activateWindow();
}

void MainWindow::discardChanges()
{
    const bool editing = m_app.settings().syncEditing();
    QString error;
    const auto summary = sync::summarizeChanges(
        m_app.database().connectionName(),
        sync::SyncService::basePath(m_app.settings().databasePath()), &error);
    if (!summary) {
        QMessageBox::warning(this, tr("Discard My Changes"), error);
        return;
    }
    if (summary->isEmpty()) {
        QMessageBox::information(this, tr("Discard My Changes"),
                                 tr("There's nothing to discard: this computer matches the last sync."));
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Discard My Changes"),
                    tr("Discard these changes and go back to the catalogue as it was at your last "
                       "sync?"),
                    QMessageBox::NoButton, this);
    box.setInformativeText(summary->headline() + QStringLiteral("\n\n")
                           + tr("PhotoLife restarts to finish. The current catalogue is kept as a "
                                "backup, so this can be undone with Restore a Backup."));
    QCheckBox *release = nullptr;
    if (editing && !m_serverView.offline) {
        release = new QCheckBox(tr("Also stop editing (let others edit)"), &box);
        box.setCheckBox(release);
    }
    QPushButton *discard = box.addButton(tr("Discard Changes"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != discard)
        return;
    if (m_reviewDialog)
        m_reviewDialog->close();
    runSync(release && release->isChecked() ? Op::DiscardAndRelease : Op::Discard, true);
}

void MainWindow::restoreBackup()
{
    auto &service = m_app.syncService();
    const QStringList backups = service.backups();
    if (backups.isEmpty()) {
        QMessageBox::information(this, tr("Restore a Backup"), tr("There are no backups yet."));
        return;
    }
    QStringList labels;
    for (const QString &path : backups) {
        const QFileInfo f(path);
        labels << tr("%1 — saved %2 — %3 MB")
                      .arg(f.fileName(), describeTime(f.lastModified()))
                      .arg(QString::number(f.size() / 1048576.0, 'f', 1));
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Restore a Backup"),
                                                 tr("Catalogue to restore (newest first):"),
                                                 labels, 0, false, &ok);
    if (!ok)
        return;
    const QString path = backups.at(labels.indexOf(chosen));
    QString text = tr("Restore %1? PhotoLife restarts to finish; the current catalogue is kept as "
                      "a backup.")
                       .arg(QFileInfo(path).fileName());
    if (m_app.settings().syncEnabled())
        text += QStringLiteral("\n\n")
                + tr("Anything in it that differs from your last sync then shows as your changes "
                     "(Review Changes). Nothing is sent until you Sync while editing.");
    if (QMessageBox::question(this, tr("Restore a Backup"), text) != QMessageBox::Yes)
        return;
    if (service.isRunning()) {
        statusBar()->showMessage(tr("The server is busy — try again in a moment."), 6000);
        return;
    }
    m_runningOp = Op::RestoreBackup;
    m_syncUserInitiated = true;
    beginTaskProgress(syncOpTitle(Op::RestoreBackup), {});
    service.restoreBackup(path);
}

void MainWindow::testConnection()
{
    runSync(Op::TestConnection, true);
}

void MainWindow::switchToLocalCatalogue()
{
    switchToCatalogue(QString::fromLatin1(Settings::kLocalProfileId));
}

void MainWindow::setUpSharedCatalogue()
{
    // A server catalogue opened here for the first time: its working copy is
    // new and empty. Get the server's catalogue -- or, for an empty server,
    // start it (from a copy of the Local catalogue, or empty).
    auto &settings = m_app.settings();
    auto &service = m_app.syncService();
    if (service.isRunning())
        return;
    m_syncSetupRunning = true;
    const QString serverName = settings.activeProfile().name;

    auto runAndWait = [&](sync::SyncService::Op op, const QString &source = {}) {
        QEventLoop loop;
        sync::SyncService::Outcome result;
        auto done = connect(&service, &sync::SyncService::finished, &loop,
                            [&](const sync::SyncService::Outcome &o) {
                                result = o;
                                loop.quit();
                            });
        auto progress = connect(&service, &sync::SyncService::progress, this,
                                [this](const QString &phase, int d, int t) { updateTaskProgress(phase, d, t); });
        beginTaskProgress(syncOpTitle(op), {});
        if (op == sync::SyncService::Op::PublishFromFile)
            service.publishFromFile(source);
        else
            service.run(op);
        loop.exec();
        disconnect(done);
        disconnect(progress);
        endTaskProgress();
        return result;
    };
    auto backToLocal = [&] {
        settings.setActiveProfile(QString::fromLatin1(Settings::kLocalProfileId));
        relaunch();
    };

    const auto check = runAndWait(sync::SyncService::Op::Check);
    if (!check.ok || !check.state) {
        QMessageBox box(QMessageBox::Warning, tr("Server Catalogue"),
                        tr("Couldn't reach \"%1\".").arg(serverName), QMessageBox::NoButton, this);
        box.setInformativeText(check.error);
        QPushButton *retry = box.addButton(tr("Try Again"), QMessageBox::AcceptRole);
        box.addButton(tr("Back to Local Catalogue"), QMessageBox::RejectRole);
        box.exec();
        m_syncSetupRunning = false;
        if (box.clickedButton() == retry)
            QTimer::singleShot(0, this, &MainWindow::setUpSharedCatalogue);
        else
            backToLocal();
        return;
    }

    const QString localPath = settings.profileDatabasePath(QString::fromLatin1(Settings::kLocalProfileId));
    const QString localSummary = ManageCataloguesDialog::describeCatalogueFile(localPath);
    QMessageBox box(QMessageBox::Question, tr("Server Catalogue: %1").arg(serverName), QString(),
                    QMessageBox::NoButton, this);
    QPushButton *download = nullptr, *fromLocal = nullptr, *empty = nullptr;
    if (check.state->empty) {
        box.setText(tr("\"%1\" is empty. How do you want to start it?").arg(serverName));
        box.setInformativeText(
            tr("From your Local catalogue (%1): a copy is published to the server; your Local "
               "catalogue itself stays as it is.\n\nOr start empty, and add photos and reference "
               "trees here.")
                .arg(localSummary));
        fromLocal = box.addButton(tr("Copy My Local Catalogue"), QMessageBox::AcceptRole);
        empty = box.addButton(tr("Start Empty"), QMessageBox::ActionRole);
    } else {
        box.setText(tr("Get the catalogue from \"%1\"?").arg(serverName));
        box.setInformativeText(
            tr("The server has %n reference tree(s)", nullptr, check.state->trees)
            + tr(" and %1 photos. A working copy is downloaded to this computer, separate from "
                 "your Local catalogue.")
                  .arg(QLocale().toString(check.state->photos)));
        download = box.addButton(tr("Download"), QMessageBox::AcceptRole);
    }
    QPushButton *back = box.addButton(tr("Back to Local Catalogue"), QMessageBox::RejectRole);
    box.setDefaultButton(back);
    box.exec();

    sync::SyncService::Outcome result;
    if (download && box.clickedButton() == download) {
        result = runAndWait(sync::SyncService::Op::Pull);
    } else if (fromLocal && box.clickedButton() == fromLocal) {
        result = runAndWait(sync::SyncService::Op::PublishFromFile, localPath);
        if (result.ok)
            settings.setSyncEditing(true);   // publishing takes the edit lock
    } else if (empty && box.clickedButton() == empty) {
        result = runAndWait(sync::SyncService::Op::Publish);
        if (result.ok)
            settings.setSyncEditing(true);
    } else {
        backToLocal();
        return;
    }
    if (!result.ok) {
        QMessageBox::warning(this, tr("Server Catalogue"), result.error);
        backToLocal();
        return;
    }
    if (result.outsideRoot > 0)
        QMessageBox::information(
            this, tr("Photos Outside the Library Folder"),
            tr("%n photo location(s) are outside this catalogue's photo library folder, so "
               "collaborators won't be able to open those photos.",
               nullptr, result.outsideRoot));
    relaunch();   // opens the downloaded / published working copy
}

void MainWindow::relaunch()
{
    if (m_app.scanService().isRunning() || m_app.matchService().isRunning()
        || m_app.lightroomImporter().isRunning() || m_builder->isRunning()
        || m_infraFiller->isRunning()) {
        m_app.scanService().cancel();
        m_app.matchService().cancel();
        m_app.lightroomImporter().cancel();
        m_builder->cancel();
        m_infraFiller->cancel();
    }
    // Force settings out to disk before the new process reads them --
    // QSettings buffers writes, which a brand-new process can't see.
    m_app.settings().sync();
    m_closeConfirmed = true;   // no "finish editing?" on the way out
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

} // namespace pl
