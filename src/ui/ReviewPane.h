#pragma once

#include "match/CandidateFinder.h"
#include "match/MatchReviewer.h"

#include <QModelIndex>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QResizeEvent;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeView;

namespace pl {
class Database;
}
namespace pl::thumb {
class ThumbnailCache;
}
namespace pl::net {
class INatClient;
class PhotoCache;
}
namespace pl::model {
class ReviewQueueModel;
class ReviewQueueGroupModel;
class ReviewQueueFilterProxy;
}

namespace pl {

class ImageViewer;

// The reconciliation screen: the pending-match queue on the left, and on the
// right the selected capture with the engine's guess, ranked alternatives, a
// taxon search box, and Confirm / Reject / Genus only / Not a taxon / Skip —
// plus a "apply to the whole folder" bulk action.
class ReviewPane : public QWidget
{
    Q_OBJECT

public:
    ReviewPane(pl::Database &db, pl::thumb::ThumbnailCache &thumbs, pl::net::INatClient &inat,
               pl::net::PhotoCache &photos, QWidget *parent = nullptr);

    void reload();
    int queueCount() const;

    // The taxon currently selected in the reference-tree dock (0 = none). Backs
    // the "Use Selected Taxon on Tree" button; pushed in by MainWindow whenever
    // the tree selection changes.
    void setTreeTaxon(qint64 inatId, const QString &name);

signals:
    void queueChanged();   // a decision was applied; refresh coverage / other views

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void showCurrent();
    void openViewerAt(const QModelIndex &groupIndex);
    void selectCapture(const QModelIndex &leaf);
    void restoreGroupExpansion();
    void collapseAllGroups();
    void expandAllGroups();
    void populateCandidates(const QList<pl::match::TaxonCandidate> &candidates,
                            qint64 preselectInatId);
    void runSearch(const QString &text);
    void updateReferencePhoto();
    void showRefPixmap(qint64 inatId, const QString &url, const QString &name,
                       const QString &attribution);
    void onRefPhotoReady(const QString &url);
    void clearReferencePhoto();
    void rescalePhotos();
    void renderCaptureInfo();
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
    void ignoreFolder();
    void useTreeTaxon();
    void updateTreeTaxonButton();
    void undo();
    void setBucket(int bucket);
    void updateBucketButtons();

    int visibleQueueCount() const;        // capture-leaf count (respects the filter)
    QModelIndex visibleQueueIndex(int row) const;

    pl::Database &m_db;
    pl::thumb::ThumbnailCache &m_thumbs;
    pl::net::INatClient &m_inat;
    pl::net::PhotoCache &m_photos;
    pl::model::ReviewQueueModel *m_queue;
    pl::model::ReviewQueueFilterProxy *m_queueProxy;
    pl::model::ReviewQueueGroupModel *m_queueGroup;
    pl::match::MatchReviewer m_reviewer;
    pl::match::CandidateFinder m_finder;

    QLineEdit *m_photoFilter;
    QButtonGroup *m_bucketGroup;
    QToolButton *m_bucketAllButton;
    QToolButton *m_bucketNoCandidateButton;
    QToolButton *m_bucketNeedsReviewButton;
    QTreeView *m_queueView;
    QSet<QString> m_collapsedGroups;   // group names the user has collapsed, kept across reloads
    QStackedWidget *m_rightStack;      // 0 = empty message, 1 = detail form
    QLabel *m_yourPhotoTitle;
    QLabel *m_refPhotoTitle;
    QLabel *m_thumb;
    QLabel *m_refThumb;
    QLabel *m_refCaption;
    QPixmap m_capturePixmapSrc;        // unscaled sources, kept for rescale-on-resize
    QPixmap m_refPixmapSrc;
    QTimer *m_refDebounce;
    qint64 m_refTaxonInatId = 0;     // taxon the reference photo currently reflects
    QString m_refPendingUrl;         // URL awaiting PhotoCache::ready
    QString m_refPendingName;
    QString m_refPendingAttr;
    QSet<qint64> m_refNoPhoto;       // taxa iNat has no default_photo for; don't re-ask
    QLabel *m_info;
    QString m_captureName;           // raw capture name / folder, kept for re-eliding
    QString m_captureFolder;
    QLabel *m_guess;
    QListWidget *m_candidates;
    QPushButton *m_confirmButton;
    QLineEdit *m_search;
    QLabel *m_bulkLabel;
    QPushButton *m_bulkButton;
    QCheckBox *m_recursiveCheck;
    QPushButton *m_ignoreFolderButton;
    QPushButton *m_useTreeTaxon;
    qint64 m_treeTaxonInatId = 0;
    QString m_treeTaxonName;
    QPushButton *m_undoButton;
    qint64 m_lastDecidedCaptureId = 0;
    QLabel *m_empty;
    ImageViewer *m_viewer = nullptr;
};

} // namespace pl
