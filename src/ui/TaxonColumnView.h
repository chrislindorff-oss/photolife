#pragma once

#include <QColumnView>
#include <QStyledItemDelegate>
#include <QWidget>

class QLabel;
class QPushButton;

namespace pl {

class Application;

// One entry in a column of the Reference Tree's Columns view:
//   Acanthiza                                  genus   7 / 8  ›
//   Australasian Thornbills
// -- the scientific name with the common name under it on the left; its rank, coverage (✔ or
// "with / total"), the Seen tick (species and lower, when shown) and an
// arrow for a taxon with more beneath it, on the right. A click on the tick
// toggles it, as in the tree's Seen column.
class TaxonColumnDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setShowSeen(bool on) { m_showSeen = on; }
    bool showSeen() const { return m_showSeen; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

    // Where the Seen tick is drawn in a row of `rect` (null when there's none).
    QRect checkRect(const QRect &rect, const QModelIndex &index) const;

private:
    bool m_showSeen = false;
};

// What's at the end of the line: the taxon's iNaturalist photo, your own
// cover photo of it when you have one, its names, rank, coverage, status
// and Seen tick, with Show Photos and Open on iNaturalist.
class TaxonCard : public QWidget
{
    Q_OBJECT

public:
    TaxonCard(Application &app, QWidget *parent = nullptr);

    // Fills the card from a TaxonomyTreeModel index; `projectId` picks your cover photo.
    void setTaxon(const QModelIndex &index, int projectId);
    qint64 taxon() const { return m_inatId; }

    // Exposed for tests.
    QString namesText() const;

signals:
    void showPhotosRequested(qint64 inatId);

private:
    void showReferencePhoto();
    void showOwnPhoto();

    Application &m_app;
    qint64 m_inatId = 0;
    QString m_photoUrl;
    QString m_ownHash;
    QString m_ownPath;
    QLabel *m_reference = nullptr;
    QLabel *m_credit = nullptr;
    QLabel *m_own = nullptr;
    QLabel *m_ownCaption = nullptr;
    QLabel *m_names = nullptr;
    QLabel *m_details = nullptr;
    QPushButton *m_showPhotos = nullptr;
    QPushButton *m_openInat = nullptr;
};

// The Reference Tree as columns (Finder's column view): click a taxon and
// what's beneath it opens in the next column, from the top of the tree down
// to the species and below. Runs on the same TaxonomyTreeModel as the tree,
// so hidden ranks and filters apply. Its TaxonCard is for the caller to place
// beside it: it shows the current species (or lower taxon) and hides itself
// above that -- a species with subspecies still has a card, which a
// QColumnView preview (only for taxa with nothing beneath) wouldn't give.
class TaxonColumnView : public QColumnView
{
    Q_OBJECT

public:
    TaxonColumnView(Application &app, QWidget *parent = nullptr);
    ~TaxonColumnView() override;

    void setShowSeen(bool on);
    TaxonColumnDelegate *taxonDelegate() const { return m_delegate; }
    TaxonCard *card() const { return m_card; }
    // The tree whose taxa are shown (for the card's cover photo).
    void setProjectId(int projectId) { m_projectId = projectId; }
    // Fills (or hides) the card for the current taxon.
    void updateCard();

signals:
    void taxonContextMenu(const QModelIndex &index, const QPoint &globalPos);
    void taxonActivated(const QModelIndex &index);

protected:
    QAbstractItemView *createColumn(const QModelIndex &rootIndex) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;

private:
    TaxonColumnDelegate *m_delegate = nullptr;
    TaxonCard *m_card = nullptr;   // parented by whoever lays it out
    int m_projectId = -1;
};

} // namespace pl
