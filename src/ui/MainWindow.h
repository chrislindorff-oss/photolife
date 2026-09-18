#pragma once

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
class QComboBox;
class QAction;
class QActionGroup;
class QAbstractItemModel;

namespace pl {

class Application;

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
    void buildCentralWidget();
    void restoreLayout();
    void showAbout();
    void showHelp();
    void showStorageUsage();
    void showCatalogueSettings();

    void addWatchedFolder();
    void startScan();
    void setScanUiRunning(bool running);
    void onScanProgress(const scan::ScanProgress &progress);
    void onScanFinished(const scan::ScanSummary &summary);
    void updateEmptyState();

    void buildReferenceTreeDock();
    QWidget *buildBrowsePage();
    QWidget *buildMapPage();
    QWidget *buildBestShotsPage();
    QWidget *buildMissingPage();
    QWidget *buildReferencePhotosPage();
    void updateReferencePhotoStatus();
    void maybeFetchReferencePhotos();
    void fetchReferencePhotos();
    QWidget *buildInatDownloadPage();
    void searchInatObservations();
    void downloadSelectedInatPhotos();
    void onInatSearchFinished(bool ok, const QString &error, QList<inat::Candidate> candidates);
    void onInatImportFinished(bool ok, const QString &error, QList<inat::SavedFile> saved);
    void updateReviewTabText();
    void updateBestShotsTabText();
    void switchToTreeTab(QWidget *page);
    void setBestShotForSelection(QListView *grid, bool nominate);
    void reloadProjectList();
    void newReferenceTree();
    void refreshReferenceTree();
    void deleteReferenceTree();
    void fetchInfraspecificTaxa();
    void startInfraspecificFetch(qint64 scopeInatId, const QString &scopeName);
    void showTreeContextMenu(const QPoint &pos);
    void importChecklist();
    void refreshCoverage();
    void onTreeSelectionChanged();
    void selectTaxonInTree(qint64 inatId);
    void onTreeSearchChanged(const QString &text);
    void onTreeSearchNext();
    QList<qint64> collectExpandedTaxa(const QModelIndex &parent = {}) const;
    void mutateTreePreservingState(const std::function<void()> &mutate);
    void rebuildRankFilterMenu();
    void updateMissingList();
    void showMissingListContextMenu(const QPoint &pos);
    void viewReferencePhoto(qint64 inatId);
    void openViewer(QAbstractItemModel *model, const QModelIndex &index);
    QListView *makeCaptureGrid(QAbstractItemModel *model);
    void removeSelectedCaptures(QListView *grid);
    void removeMissingCaptures();
    void fixRawGeolocation();
    void fetchPhotoLocalities();
    int currentProjectId() const;
    void startMatch();
    void reassignTaxonPhotos();
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

    model::CaptureListModel *m_taxonModel = nullptr;
    QListView *m_taxonGrid = nullptr;
    QLabel *m_taxonRepImage = nullptr;
    QLabel *m_taxonInfo = nullptr;
    MapView *m_mapView = nullptr;

    model::CaptureListModel *m_bestShotModel = nullptr;
    QListView *m_bestShotGrid = nullptr;
    QLabel *m_bestShotHeader = nullptr;
    QListWidget *m_missingList = nullptr;
    QLineEdit *m_missingSearch = nullptr;
    ReferencePhotoDialog *m_referencePhotoDialog = nullptr;

    model::ReferencePhotoModel *m_refPhotoModel = nullptr;
    QListView *m_refPhotoGrid = nullptr;
    QLabel *m_refPhotoStatus = nullptr;
    QWidget *m_refPhotoPage = nullptr;
    taxonomy::ReferencePhotoFetcher *m_refPhotoFetcher = nullptr;
    QAction *m_fetchRefPhotosAction = nullptr;
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
    QLabel *m_inatStatus = nullptr;
    QPushButton *m_inatSearchButton = nullptr;
    QPushButton *m_inatCancelSearchButton = nullptr;
    QPushButton *m_inatDownloadButton = nullptr;
    QString m_inatPendingDestFolder;   // set while an import's post-scan steps are still pending

    CoveragePanel *m_coveragePanel = nullptr;
    QAction *m_newTreeAction = nullptr;
    QAction *m_refreshTreeAction = nullptr;
    QAction *m_deleteTreeAction = nullptr;
    QAction *m_fetchInfraAction = nullptr;
    QAction *m_importChecklistAction = nullptr;

    QAction *m_matchAction = nullptr;
    QComboBox *m_filterCombo = nullptr;
};

} // namespace pl
