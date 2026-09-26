#pragma once

#include "inat/SpeciesComparison.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <utility>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QToolButton;
class QTreeView;

namespace pl {

class Application;
class ComparisonFilterProxy;
class InatAccountBar;
class ReferenceTreeCombo;
class TaxonLimitRow;

namespace inat {
class InatSpeciesListFetcher;
}

// "Compare Species": an iNaturalist observer's taxa at one rank (optionally
// within one group) side by side with the reference trees' taxa and the
// photo library -- above all, what's on iNaturalist but missing from a tree
// or the library.
class InatComparePage : public QWidget
{
    Q_OBJECT

public:
    InatComparePage(Application &app, InatAccountBar &account, QWidget *parent = nullptr);

    void setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId);
    void followActiveTree(int projectId);
    void setTreeSelection(qint64 inatId, const QString &name);

signals:
    void statusMessage(const QString &text, int timeoutMs);
    void addToTreeRequested(int projectId, qint64 inatId, const QString &name);
    void showInTreeRequested(int projectId, qint64 inatId);
    void downloadTaxonRequested(qint64 inatId, const QString &label);

private:
    void updateLocalityEnabled();
    void compare();
    void onFetchFinished(bool ok, const QString &error, QList<net::SpeciesCount> inatLeaves,
                         QHash<qint64, taxonomy::Taxon> known);
    void showResult();
    void showContextMenu(const QPoint &pos);
    void exportCsv();
    QString subsetLabel(int subset) const;

    Application &m_app;
    InatAccountBar &m_account;
    inat::InatSpeciesListFetcher *m_fetcher = nullptr;

    // The settings the results in the table were produced with.
    QList<qint64> m_pendingTreeLeaves;
    QString m_resultRank;
    QString m_resultObserver;
    int m_resultProjectId = -1;
    QString m_resultGroupLabel;   // empty = every taxon
    inat::ComparisonResult m_result;

    QLineEdit *m_observerEdit = nullptr;
    bool m_observerEdited = false;
    ReferenceTreeCombo *m_treeCombo = nullptr;
    QComboBox *m_rankCombo = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QCheckBox *m_restrictToLocality = nullptr;
    QPushButton *m_compareButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    TaxonLimitRow *m_taxonLimit = nullptr;
    QList<QToolButton *> m_subsetButtons;   // index = Subset (see the .cpp)
    QLabel *m_note = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QComboBox *m_showCombo = nullptr;
    QPushButton *m_exportButton = nullptr;
    QStandardItemModel *m_model = nullptr;
    ComparisonFilterProxy *m_proxy = nullptr;
    QTreeView *m_table = nullptr;
};

} // namespace pl
