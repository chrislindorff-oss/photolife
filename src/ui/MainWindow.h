#pragma once

#include "catalogue/UntreedMatchStore.h"
#include "coverage/CoverageCalculator.h"
#include "sync/SyncService.h"
#include "ui/ServerBar.h"

#include <QPointer>

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
class QTimer;
class QComboBox;
class QAction;
class QActionGroup;
class QAbstractItemModel;
class QProgressDialog;

namespace pl {

class ActivityLogDialog;
class ReviewChangesDialog;
class ServerStatusDialog;

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
class OriginFetcher;
class AlaStatusSync;
class ApcOriginSync;
class InfraspecificFiller;
class ReferencePhotoFetcher;
}
namespace match {
class MatchEngine;
struct StaleDecision;
}
namespace checklist {
class ChecklistImporter;
}
namespace treefile {
class TreeImporter;
}
namespace collection {
struct ExportSummary;
}
namespace geo {
class LocalityFetcher;
}
namespace inat {
class InatImportService;
struct ImportItem;
struct SavedFile;
}

class CoveragePanel;
class HelpWindow;
class ImageViewer;
class InatToolsView;
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
    void buildInatMenu(QMenu *inatMenu);
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
    // Starts the pipeline below for the photos the iNaturalist view's
    // Download My Observations page asked for.
    void downloadSelectedInatPhotos(const QList<inat::ImportItem> &items);
    void onInatImportFinished(bool ok, const QString &error, QList<inat::SavedFile> saved);
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
    // Fetches iNaturalist place ancestry and outlines for tree regions that
    // lack them.
    void fetchMissingPlaceDetails();
    // Threatened status (ALA) and plant origins (APC) for Australian trees:
    // when due (monthly), when `force`d from the menu, or -- `rematchOnly`,
    // after a tree gains taxa -- re-matched against what's already fetched.
    void syncAustralianSources(bool force, bool rematchOnly);
    bool hasAustralianTree() const;
    // Reference Tree menu: re-downloads both, in a progress window.
    void updateAustralianSourcesNow();
    void updateSourceSyncProgress();
    void sourceSyncPartFinished();
    void newReferenceTree();
    void refreshReferenceTree();
    void addTaxonToReferenceTree();
    // Fetches `inatId` (with its ancestors) from iNaturalist and links it into
    // `projectId`'s tree -- shared by Add Taxon and the "Not in Any Tree" tab.
    void addTaxonToTree(int projectId, qint64 inatId, const QString &name);
    void deleteReferenceTree();
    void renameReferenceTree();
    // Right-click on the tree selector: Rename, plus the tree's own commands.
    void showTreeSelectorMenu(const QPoint &pos);
    void fetchInfraspecificTaxa();
    void startInfraspecificFetch(qint64 scopeInatId, const QString &scopeName);
    void showTreeContextMenu(const QPoint &pos);
    void pruneTaxonFromTree(qint64 inatId, const QString &name);
    void importChecklist();
    void exportReferenceTree();
    void importReferenceTree();
    void importLightroom();

    // Probes a Postgres catalogue on a background thread (Database::
    // probeReachable(), the same mechanism the startup connect dialog
    // already uses) before a long Postgres-writing action starts, so a
    // suspended Neon compute wakes up before the GUI thread makes its first
    // *synchronous* write -- otherwise that single blocking call can run
    // long enough for the OS to decide PhotoLife has hung. A no-op (returns
    // true immediately) when the active backend is SQLite.
    bool ensureCatalogueReachable();

    // --- shared catalogue (sync mode) ---
    // True when this machine may change the catalogue. Otherwise explains
    // that the shared catalogue is read-only here, offers to Start Editing,
    // and returns false -- call it first in anything that writes.
    bool ensureCanEdit();
    void setupSync();
    void runSync(sync::SyncService::Op op, bool userInitiated);
    void onSyncFinished(const sync::SyncService::Outcome &outcome);
    void syncNow();
    void startEditing(bool breakLock = false);
    void finishEditing();
    void offerBreakLock();
    void applyEditability();
    // Recomputes m_serverView's derived parts and shows it everywhere: the
    // top-right bar, the Server Status window, the menu, the window title.
    void updateSyncStatus();
    void buildServerMenu(QMenu *serverMenu);
    void showServerStatus();
    void showReviewChanges();
    void showActivityLog();
    void discardChanges();
    void restoreBackup();
    void testConnection();
    void switchToLocalCatalogue();
    // Opens another catalogue profile (restarting PhotoLife), offering to
    // Finish Editing first when leaving a server catalogue mid-edit.
    void switchToCatalogue(const QString &id);
    void editServerSettings();
    // The top-right corner: the catalogue switcher, then the LOCAL badge or
    // the ServerBar.
    void buildCatalogueCorner();
    void updateCatalogueCorner();
    // Counts the unsent changes again, a moment after edits (local only).
    void scheduleChangesCount();
    // "Send these changes?" -- true to go ahead.
    bool confirmSend(bool finishing);
    void logActivity(const sync::SyncService::Outcome &outcome);
    // First use of a shared catalogue: download it, or publish this one.
    void setUpSharedCatalogue();
    // Restarts PhotoLife (settings saved first), e.g. to open a downloaded
    // catalogue.
    void relaunch();
    // A progress window (with Cancel) for a scan the user started -- not for
    // the folder watcher's quiet background rescans.
    void beginScanProgress(const QString &title);

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

    // Recomputes the active tree's coverage (panel, tree badges, map
    // locality, photo tabs). Queued: every request made while handling one
    // action runs as a single refresh on the next event-loop turn -- each
    // refresh is several catalogue queries, which a remote (Postgres)
    // catalogue pays for in round trips.
    void refreshCoverage();
    void refreshCoverageNow();
    void onTreeSelectionChanged();
    void selectTaxonInTree(qint64 inatId);
    void onTreeSearchChanged(const QString &text);
    void onTreeSearchNext();
    QList<qint64> collectExpandedTaxa(const QModelIndex &parent = {}) const;
    void mutateTreePreservingState(const std::function<void()> &mutate);
    void applyThreatenedFilter(bool anyThreatened, const QString &status);
    // Tree filter to the tree's introduced species (from the coverage
    // panel's introduced summary); replaces any threatened filter.
    void applyIntroducedFilter(bool on);
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
    // Rescans the library, then calls `startMatching` -- unless the scan was
    // stopped or failed. See the .cpp for why matching always scans first.
    void scanThenMatch(std::function<void()> startMatching);
    // The progress window for Match Library / Match Library Against a group:
    // the pre-match scan, then matching. Cancel stops whichever is running.
    void beginMatchProgress(const QString &title);
    void endMatchProgress();
    // Match Library Against This Group: re-checks unresolved photos (see
    // MatchEngine::matchGroup) against the tree's taxa at/below `inatId`.
    void startGroupMatch(qint64 inatId, const QString &name);
    // After a match run: asks what to do about photos identified by the user
    // whose file names now point to a different taxon (see StaleMatchDialog).
    void resolveStaleDecisions(const QList<match::StaleDecision> &stale);
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
    QAction *m_viewInatAction = nullptr;
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
    bool m_coverageRefreshQueued = false;

    QMenu *m_serverMenu = nullptr;
    ServerBar *m_serverBar = nullptr;
    ServerView m_serverView;
    QPointer<ServerStatusDialog> m_statusDialog;
    QPointer<ReviewChangesDialog> m_reviewDialog;
    QPointer<ActivityLogDialog> m_activityDialog;
    QTimer *m_changesTimer = nullptr;
    std::optional<sync::SyncService::Op> m_runningOp;
    std::optional<sync::SyncService::Op> m_queuedOp;   // asked for while counting changes
    QAction *m_syncNowAction = nullptr;
    QAction *m_serverStatusAction = nullptr;
    QAction *m_reviewChangesAction = nullptr;
    QAction *m_discardAction = nullptr;
    QAction *m_testConnectionAction = nullptr;
    QAction *m_restoreBackupAction = nullptr;
    QAction *m_startEditingAction = nullptr;
    QAction *m_finishEditingAction = nullptr;
    QAction *m_breakLockAction = nullptr;
    QTimer *m_syncCheckTimer = nullptr;
    std::optional<sync::ServerState> m_serverState;
    qint64 m_localSyncVersion = 0;
    int m_pendingChanges = -1;
    bool m_syncUserInitiated = false;
    bool m_closeAfterSync = false;   // closing waits on a Finish Editing
    bool m_closeConfirmed = false;   // ...which has now finished
    bool m_syncSetupRunning = false; // setUpSharedCatalogue() handles its own results
    QString m_switchAfterSync;       // a catalogue to open once Finish Editing is done
    QWidget *m_catalogueCorner = nullptr;
    QToolButton *m_catalogueSwitcher = nullptr;
    QLabel *m_localBadge = nullptr;
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
    QComboBox *m_coverageFilterCombo = nullptr;   // all / fully covered / not fully covered
    QToolButton *m_rankFilterButton = nullptr;
    QMenu *m_rankFilterMenu = nullptr;
    QHash<QString, QCheckBox *> m_rankCheckboxes;   // rank -> its "show this rank" checkbox
    QStringList m_rankFilterOrder;                  // ranks currently backing the menu
    QSet<QString> m_hiddenRanks;
    taxonomy::ProjectBuilder *m_builder = nullptr;
    bool m_builderIsNewTree = false;   // distinguishes New from Refresh in m_builder::finished
    taxonomy::InfraspecificFiller *m_infraFiller = nullptr;
    taxonomy::OriginFetcher *m_originFetcher = nullptr;
    taxonomy::AlaStatusSync *m_alaSync = nullptr;
    taxonomy::ApcOriginSync *m_apcSync = nullptr;
    // The progress window for a user-requested update (updateAustralianSourcesNow).
    struct SourceSyncProgress
    {
        bool active = false;   // the window is up
        bool alaDone = false;
        bool apcDone = false;
        int alaIndex = 0;
        int alaCount = 0;
        double apcFraction = 0.0;
        QString alaLine;
        QString apcLine;
    };
    SourceSyncProgress m_sourceSync;
    checklist::ChecklistImporter *m_checklistImporter = nullptr;
    treefile::TreeImporter *m_treeImporter = nullptr;
    geo::LocalityFetcher *m_localityFetcher = nullptr;
    QAction *m_fetchLocalitiesAction = nullptr;

    InatToolsView *m_inatView = nullptr;
    inat::InatImportService *m_inatImportService = nullptr;
    bool m_inatPipelineActive = false;
    bool m_inatPipelineCancelled = false;
    QList<inat::ImportItem> m_inatPendingItems;   // the running download's selection
    QString m_inatPendingDestFolder;   // set while an import's post-scan steps are still pending
    bool m_rescanQueued = false;       // see requestScan()
    bool m_lastScanCompleted = false;  // the last scan finished without error or Stop
    bool m_matchAfterScanPending = false;   // scanThenMatch() is waiting on its scan
    bool m_matchProgressActive = false;     // beginMatchProgress()'s window is up
    bool m_matchCancelled = false;          // ...and its Cancel was pressed
    QList<std::function<void()>> m_afterQueuedScan;

    CoveragePanel *m_coveragePanel = nullptr;
    QAction *m_newTreeAction = nullptr;
    QAction *m_refreshTreeAction = nullptr;
    QAction *m_addTaxonAction = nullptr;
    QAction *m_deleteTreeAction = nullptr;
    QAction *m_fetchInfraAction = nullptr;
    QAction *m_importChecklistAction = nullptr;
    QAction *m_importTreeAction = nullptr;
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
    bool m_scanProgressActive = false;   // a user-started scan owns m_taskProgress

    QAction *m_matchAction = nullptr;
    QString m_groupMatchName;   // set while a group match runs; empty for a full Match Library
    bool m_matchingDownload = false;   // set while matching just an iNaturalist download's photos
    QComboBox *m_filterCombo = nullptr;
};

} // namespace pl
