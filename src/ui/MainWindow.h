#pragma once

#include "catalogue/UntreedMatchStore.h"
#include "coverage/CoverageCalculator.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QModelIndex>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QMenu;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QAction;
class QActionGroup;
class QAbstractItemModel;
class QProgressDialog;

namespace pl {

class Application;
class PostgresConnectionMonitor;

namespace model {
class CaptureListModel;
class ReferencePhotoModel;
class TaxonomyTreeModel;
}
namespace scan {
struct ScanProgress;
struct ScanSummary;
}
namespace taxonomy {
class ProjectBuilder;
class InfraspecificFiller;
class ReferencePhotoFetcher;
}
namespace match {
class MatchEngine;
}
namespace checklist {
class ChecklistImporter;
}
namespace collection {
struct ExportSummary;
}
namespace geo {
class LocalityFetcher;
}
namespace inat {
class InatObservationFetcher;
class InatDownloadModel;
class InatImportService;
struct Candidate;
struct SavedFile;
}

class CoveragePanel;
class HelpWindow;
class ImageViewer;
class InatFilterProxyModel;
class MapView;
class ReferencePhotoDialog;
class ReviewPane;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Application &app, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void buildFileMenu(QMenu *fileMenu);
    void buildLibraryMenu(QMenu *libraryMenu);
    void buildReferenceTreeMenu(QMenu *treeMenu);
    void buildViewMenu();
    void buildMatchingMenu(QMenu *matchingMenu);
    void buildHelpMenu(QMenu *helpMenu);
    void buildToolBar();
    void buildCentralWidget();
    void restoreLayout();
    void showAbout();
    void showHelp();
    void showStorageUsage();
    void showCatalogueSettings();

    void addWatchedFolder();
    void manageLibraryFolders();
    void startScan();
    // Like startScan(), but never dropped: if a scan is already running (e.g.
    // one the folder watcher started mid-download), another full scan is
    // queued to run as soon as it finishes, so files written after that scan
    // began are still picked up. `afterScan` runs once that scan completes.
    void requestScan(std::function<void()> afterScan = {});
    void runQueuedScan();
    void setScanUiRunning(bool running);
    void onScanProgress(const scan::ScanProgress &progress);
    void onScanFinished(const scan::ScanSummary &summary);
    void updateEmptyState();

    void buildReferenceTreeDock();
    QWidget *buildBrowsePage();
    QWidget *buildMapPage();
    QWidget *buildBestShotsPage();
    QWidget *buildMissingPage();
    QWidget *buildUntreedPage();
    QWidget *buildReferencePhotosPage();
    void updateReferencePhotoStatus();
    void maybeFetchReferencePhotos();
    void fetchReferencePhotos();
    void exportReferenceTreePhotos();
    // After an update-mode export: lists files in the collection that this
    // run didn't produce (see ExportOptions::reportUnexpectedFiles).
    void showUnexpectedExportFiles(const collection::ExportSummary &summary);
    QWidget *buildInatDownloadPage();
    void searchInatObservations();
    void downloadSelectedInatPhotos();
    void onInatSearchFinished(bool ok, const QString &error, QList<inat::Candidate> candidates);
    void onInatImportFinished(bool ok, const QString &error, QList<inat::SavedFile> saved);
    // Annotates m_inatCandidates against the trees and library, refills the
    // grid and summarises the result in the page's status line.
    void checkInatCandidatesAgainstLibrary();
    // The locked-down download pipeline (download -> add to library -> match
    // just those photos), shown in one modal progress window. Cancel stops
    // the current step and skips the rest; photos already downloaded are
    // still added to the library so nothing on disk is left untracked.
    void cancelInatPipeline();
    void finishInatPipeline(const QString &message);
    void updateReviewTabText();
    void updateBestShotsTabText();
    void switchToTreeTab(QWidget *page);
    void setBestShotForSelection(QListView *grid, bool nominate);
    void reloadProjectList();
    void newReferenceTree();
    void refreshReferenceTree();
    void addTaxonToReferenceTree();
    // Fetches `inatId` (with its ancestors) from iNaturalist and links it into
    // `projectId`'s tree -- shared by Add Taxon and the "Not in Any Tree" tab.
    void addTaxonToTree(int projectId, qint64 inatId, const QString &name);
    void deleteReferenceTree();
    void fetchInfraspecificTaxa();
    void startInfraspecificFetch(qint64 scopeInatId, const QString &scopeName);
    void showTreeContextMenu(const QPoint &pos);
    void pruneTaxonFromTree(qint64 inatId, const QString &name);
    void importChecklist();
    void importLightroom();

    // Probes a Postgres catalogue on a background thread (Database::
    // probeReachable(), the same mechanism the startup connect dialog
    // already uses) before a long Postgres-writing action starts, so a
    // suspended Neon compute wakes up before the GUI thread makes its first
    // *synchronous* write -- otherwise that single blocking call can run
    // long enough for the OS to decide PhotoLife has hung. A no-op (returns
    // true immediately) when the active backend is SQLite.
    bool ensureCatalogueReachable();

    // Shared modal progress display for the long Postgres-writing actions
    // (reference tree build/refresh, add taxon, fetch subspecies/varieties,
    // fetch reference photos, import checklist), replacing status-bar-only
    // feedback with a persistent, clearly-updating window. Shows a Cancel
    // button wired to `onCancel` when given; omit it for a task with no
    // meaningful way to cancel mid-flight (e.g. one single network call).
    void beginTaskProgress(const QString &title, std::function<void()> onCancel = {});
    // total <= 0 shows a busy (indeterminate) bar -- for phases with no
    // known item count (e.g. resolving a taxon/place, or a single fetch).
    void updateTaskProgress(const QString &phase, int done, int total);
    void endTaskProgress();

    void refreshCoverage();
    void onTreeSelectionChanged();
    void selectTaxonInTree(qint64 inatId);
    void onTreeSearchChanged(const QString &text);
    void onTreeSearchNext();
    QList<qint64> collectExpandedTaxa(const QModelIndex &parent = {}) const;
    void mutateTreePreservingState(const std::function<void()> &mutate);
    void applyThreatenedFilter(bool anyThreatened, const QString &status);
    void rebuildRankFilterMenu();
    void updateMissingList();
    void showMissingListContextMenu(const QPoint &pos);
    // "Not in Any Tree" tab: reloadUntreedTaxa() re-runs the catalogue query
    // (after anything that can change matches or tree membership);
    // updateUntreedList() just repopulates the list from the cached result
    // (search / rank filter changes).
    void reloadUntreedTaxa();
    void updateUntreedList();
    void updateUntreedGridScope();
    void showUntreedListContextMenu(const QPoint &pos);
    void addUntreedTaxonToTree(QTreeWidgetItem *item, int projectId);
    void viewReferencePhoto(qint64 inatId);
    void openViewer(QAbstractItemModel *model, const QModelIndex &index);
    QListView *makeCaptureGrid(QAbstractItemModel *model);
    void removeSelectedCaptures(QListView *grid);
    void fetchCoordinatesFromInat(const QModelIndex &idx);
    void removeMissingCaptures();
    void fixRawGeolocation();
    void fetchPhotoLocalities();
    int currentProjectId() const;
    void startMatch();
    // Match Library Against This Group: re-checks unresolved photos (see
    // MatchEngine::matchGroup) against the tree's taxa at/below `inatId`.
    void startGroupMatch(qint64 inatId, const QString &name);
    // Reassigns the photos selected in `grid` (or, with nothing selected and
    // after confirming, every photo `model` shows) to a taxon the user picks.
    void reassignPhotos(QListView *grid, model::CaptureListModel *model);
    void applyCaptionFields(int fields);

    Application &m_app;

    model::CaptureListModel *m_model = nullptr;
    QListView *m_grid = nullptr;
    QLabel *m_emptyHint = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTabWidget *m_tabs = nullptr;
    QStackedWidget *m_photoStack = nullptr;
    QStackedWidget *m_centralStack = nullptr;
    QMenu *m_viewMenu = nullptr;
    QActionGroup *m_viewModeGroup = nullptr;
    QAction *m_viewTreeAction = nullptr;
    QAction *m_viewLibraryAction = nullptr;
    QAction *m_viewReviewAction = nullptr;
    QAction *m_darkModeAction = nullptr;
    QLabel *m_pgStatusLabel = nullptr;
    PostgresConnectionMonitor *m_pgMonitor = nullptr;

    model::CaptureListModel *m_taxonModel = nullptr;
    QListView *m_taxonGrid = nullptr;
    QLabel *m_taxonRepImage = nullptr;
    QLabel *m_taxonInfo = nullptr;
    MapView *m_mapView = nullptr;

    model::CaptureListModel *m_bestShotModel = nullptr;
    QListView *m_bestShotGrid = nullptr;
    QLabel *m_bestShotHeader = nullptr;
    QTreeWidget *m_missingList = nullptr;
    QLineEdit *m_missingSearch = nullptr;

    QList<catalogue::UntreedTaxon> m_untreedTaxa;   // last reloadUntreedTaxa() result
    QTreeWidget *m_untreedList = nullptr;
    QLineEdit *m_untreedSearch = nullptr;
    // Index = UntreedMode (see MainWindow.cpp): not in any tree / not in the
    // selected tree / identified only to genus or higher.
    QComboBox *m_untreedMode = nullptr;
    QLabel *m_untreedHeader = nullptr;
    QPushButton *m_untreedAddButton = nullptr;
    model::CaptureListModel *m_untreedModel = nullptr;
    QListView *m_untreedGrid = nullptr;
    ReferencePhotoDialog *m_referencePhotoDialog = nullptr;

    model::ReferencePhotoModel *m_refPhotoModel = nullptr;
    QListView *m_refPhotoGrid = nullptr;
    QLabel *m_refPhotoStatus = nullptr;
    QWidget *m_refPhotoPage = nullptr;
    taxonomy::ReferencePhotoFetcher *m_refPhotoFetcher = nullptr;
    QAction *m_fetchRefPhotosAction = nullptr;
    QAction *m_exportTreePhotosAction = nullptr;
    ReviewPane *m_reviewPane = nullptr;
    ImageViewer *m_viewer = nullptr;
    HelpWindow *m_helpWindow = nullptr;
    coverage::ProjectCoverage m_coverage;
    qint64 m_selectedTaxon = 0;
    qint64 m_lastSelectedTaxon = 0;   // last taxon the user chose; survives a filter toggle
    bool m_restoringTreeState = false;
    QAction *m_scanAction = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_addFolderAction = nullptr;
    QAction *m_manageFoldersAction = nullptr;

    model::TaxonomyTreeModel *m_treeModel = nullptr;
    QTreeView *m_treeView = nullptr;
    QComboBox *m_projectCombo = nullptr;
    QLineEdit *m_treeSearch = nullptr;
    QList<qint64> m_treeSearchHits;   // current search matches, best first
    int m_treeSearchPos = 0;          // which hit is focused (Enter cycles)
    QCheckBox *m_photographedOnly = nullptr;
    QToolButton *m_rankFilterButton = nullptr;
    QMenu *m_rankFilterMenu = nullptr;
    QHash<QString, QCheckBox *> m_rankCheckboxes;   // rank -> its "show this rank" checkbox
    QStringList m_rankFilterOrder;                  // ranks currently backing the menu
    QSet<QString> m_hiddenRanks;
    taxonomy::ProjectBuilder *m_builder = nullptr;
    bool m_builderIsNewTree = false;   // distinguishes New from Refresh in m_builder::finished
    taxonomy::InfraspecificFiller *m_infraFiller = nullptr;
    checklist::ChecklistImporter *m_checklistImporter = nullptr;
    geo::LocalityFetcher *m_localityFetcher = nullptr;
    QAction *m_fetchLocalitiesAction = nullptr;

    inat::InatObservationFetcher *m_inatObservationFetcher = nullptr;
    inat::InatDownloadModel *m_inatDownloadModel = nullptr;
    InatFilterProxyModel *m_inatProxyModel = nullptr;
    inat::InatImportService *m_inatImportService = nullptr;
    QWidget *m_inatDownloadPage = nullptr;
    QListView *m_inatGrid = nullptr;
    QLineEdit *m_inatUsernameEdit = nullptr;
    QLineEdit *m_inatTokenEdit = nullptr;
    QCheckBox *m_inatRestrictToLocality = nullptr;
    QLineEdit *m_inatFilterEdit = nullptr;
    QComboBox *m_inatSortCombo = nullptr;
    QCheckBox *m_inatHideFaded = nullptr;
    QComboBox *m_inatScopeCombo = nullptr;     // selected tree's taxa / all my observations
    QComboBox *m_inatPresenceCombo = nullptr;  // Show: all / new to library / not in any tree
    QPushButton *m_inatCheckButton = nullptr;  // re-check results against trees and library
    QList<inat::Candidate> m_inatCandidates;   // last search's results, for re-checking
    bool m_inatPipelineActive = false;
    bool m_inatPipelineCancelled = false;
    QLabel *m_inatStatus = nullptr;
    QPushButton *m_inatSearchButton = nullptr;
    QPushButton *m_inatCancelSearchButton = nullptr;
    QPushButton *m_inatDownloadButton = nullptr;
    QString m_inatPendingDestFolder;   // set while an import's post-scan steps are still pending
    bool m_rescanQueued = false;       // see requestScan()
    QList<std::function<void()>> m_afterQueuedScan;

    CoveragePanel *m_coveragePanel = nullptr;
    QAction *m_newTreeAction = nullptr;
    QAction *m_refreshTreeAction = nullptr;
    QAction *m_addTaxonAction = nullptr;
    QAction *m_deleteTreeAction = nullptr;
    QAction *m_fetchInfraAction = nullptr;
    QAction *m_importChecklistAction = nullptr;
    QAction *m_importLightroomAction = nullptr;
    // Set while addTaxonToReferenceTree()'s fetchTaxon() call is in flight --
    // that call is async and non-modal (unlike the TaxonConfirmDialog that
    // precedes it), so this closes the window where Delete/Refresh/Prune
    // could otherwise run against the same project before the write lands.
    bool m_addingTaxon = false;
    // Owned by beginTaskProgress()/endTaskProgress() -- recreated per task
    // rather than reused, since each task's Cancel button needs to be wired
    // to a different service's cancel().
    QProgressDialog *m_taskProgress = nullptr;

    QAction *m_matchAction = nullptr;
    QString m_groupMatchName;   // set while a group match runs; empty for a full Match Library
    bool m_matchingDownload = false;   // set while matching just an iNaturalist download's photos
    QComboBox *m_filterCombo = nullptr;
};

} // namespace pl
