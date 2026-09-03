#pragma once

#include "match/CandidateFinder.h"
#include "match/MatchReviewer.h"

#include <QModelIndex>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPushButton;
class QSortFilterProxyModel;

namespace pl {
class Database;
}
namespace pl::thumb {
class ThumbnailCache;
}
namespace pl::model {
class ReviewQueueModel;
}

namespace pl {

// The reconciliation screen: the pending-match queue on the left, and on the
// right the selected capture with the engine's guess, ranked alternatives, a
// taxon search box, and Confirm / Reject / Genus only / Not a taxon / Skip —
// plus a "apply to the whole folder" bulk action.
class ReviewPane : public QWidget
{
    Q_OBJECT

public:
    ReviewPane(pl::Database &db, pl::thumb::ThumbnailCache &thumbs, QWidget *parent = nullptr);

    void reload();
    int queueCount() const;

signals:
    void queueChanged();   // a decision was applied; refresh coverage / other views

private:
    void showCurrent();
    void populateCandidates(const QList<pl::match::TaxonCandidate> &candidates,
                            qint64 preselectInatId);
    void runSearch(const QString &text);
    qint64 currentCaptureId() const;
    int currentFolderId() const;
    qint64 chosenTaxonInatId() const;
    void afterDecision(qint64 captureId);

    void confirm();
    void reject();
    void genusOnly();
    void notATaxon();
    void skip();
    void applyToFolder();

    int visibleQueueCount() const;        // proxy row count (respects the filter)
    QModelIndex visibleQueueIndex(int row) const;

    pl::Database &m_db;
    pl::thumb::ThumbnailCache &m_thumbs;
    pl::model::ReviewQueueModel *m_queue;
    QSortFilterProxyModel *m_queueProxy;
    pl::match::MatchReviewer m_reviewer;
    pl::match::CandidateFinder m_finder;

    QLineEdit *m_photoFilter;
    QListView *m_queueView;
    QLabel *m_thumb;
    QLabel *m_info;
    QLabel *m_guess;
    QListWidget *m_candidates;
    QLineEdit *m_search;
    QLabel *m_bulkLabel;
    QPushButton *m_bulkButton;
    QLabel *m_empty;
};

} // namespace pl
