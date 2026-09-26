#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include <utility>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;

namespace pl {

class Application;
class InatAccountBar;
class InatFilterProxyModel;
class ReferenceTreeCombo;
class TaxonLimitRow;

namespace inat {
class InatDownloadModel;
class InatObservationFetcher;
struct Candidate;
struct ImportItem;
}

// "Download My Observations": searches the account's own iNaturalist
// observations (optionally limited to one reference tree's species and/or
// locality, and/or to one taxonomic group), shows the photos as a selectable grid flagged against the
// library, and hands the selection to MainWindow -- which owns the
// download -> add to library -> match pipeline, since that needs the scan
// and match services and locks the rest of the app while it runs.
class InatDownloadPage : public QWidget
{
    Q_OBJECT

public:
    InatDownloadPage(Application &app, InatAccountBar &account, QWidget *parent = nullptr);

    // Repopulates the Tree combo from (project id, display name) pairs,
    // keeping the current choice where it still exists.
    void setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId);
    // The Tree combo follows the Reference Trees dock's active tree until the
    // user picks one here themselves.
    void followActiveTree(int projectId);
    // The taxon selected in the Reference Trees dock (0 = none), offered by
    // the "Use Tree Selection" button. Pushed in by MainWindow whenever the
    // tree selection changes.
    void setTreeSelection(qint64 inatId, const QString &name);
    // Hand-off from another tool: search all the account's observations of
    // `inatId` and everything beneath it.
    void limitToTaxon(qint64 inatId, const QString &label);

    // Annotates the last search's results against the trees and library,
    // refills the grid and summarises the result in the status line.
    void recheckAgainstLibrary();
    void setDownloadEnabled(bool enabled);
    // Replaces the grid selection with the photos among `photoIds` that are
    // still shown.
    void selectPhotos(const QSet<qint64> &photoIds);

signals:
    void statusMessage(const QString &text, int timeoutMs);
    void downloadRequested(const QList<inat::ImportItem> &items);

private:
    int selectedProjectId() const;   // -1 when no tree is chosen
    void updateLocalityEnabled();
    void search();
    void onSearchFinished(bool ok, const QString &error, QList<inat::Candidate> candidates);
    void requestDownload();

    Application &m_app;
    InatAccountBar &m_account;

    inat::InatObservationFetcher *m_fetcher = nullptr;
    inat::InatDownloadModel *m_model = nullptr;
    InatFilterProxyModel *m_proxy = nullptr;
    QList<inat::Candidate> m_candidates;   // last search's results, for re-checking

    ReferenceTreeCombo *m_treeCombo = nullptr;
    TaxonLimitRow *m_taxonLimit = nullptr;
    QComboBox *m_scopeCombo = nullptr;       // chosen tree's species / all my observations
    QCheckBox *m_restrictToLocality = nullptr;
    QPushButton *m_searchButton = nullptr;
    QPushButton *m_cancelSearchButton = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QComboBox *m_sortCombo = nullptr;
    QComboBox *m_presenceCombo = nullptr;    // Show: all / new to library / not in any tree
    QCheckBox *m_hideFaded = nullptr;
    QPushButton *m_checkButton = nullptr;    // re-check results against trees and library
    QLabel *m_status = nullptr;
    QListView *m_grid = nullptr;
    QPushButton *m_downloadButton = nullptr;
};

} // namespace pl
