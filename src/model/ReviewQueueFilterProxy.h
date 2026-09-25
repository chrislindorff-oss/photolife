#pragma once

#include <QSortFilterProxyModel>

namespace pl::model {

// Narrows the review queue by whether the engine found any taxon candidate at
// all, on top of the existing text-filter behavior of QSortFilterProxyModel.
class ReviewQueueFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    enum class Bucket { All, NoCandidate, NeedsReview };

    explicit ReviewQueueFilterProxy(QObject *parent = nullptr);

    void setBucket(Bucket bucket);
    Bucket bucket() const { return m_bucket; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    Bucket m_bucket = Bucket::All;
};

} // namespace pl::model
