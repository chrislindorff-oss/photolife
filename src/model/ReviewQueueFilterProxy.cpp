#include "model/ReviewQueueFilterProxy.h"

#include "model/ReviewQueueModel.h"

namespace pl::model {

ReviewQueueFilterProxy::ReviewQueueFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
}

void ReviewQueueFilterProxy::setBucket(Bucket bucket)
{
    if (m_bucket == bucket)
        return;
    m_bucket = bucket;
    invalidateFilter();
}

bool ReviewQueueFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent))
        return false;
    if (m_bucket == Bucket::All)
        return true;

    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const bool hasCandidate = idx.data(ReviewQueueModel::HasCandidateRole).toBool();
    return m_bucket == Bucket::NoCandidate ? !hasCandidate : hasCandidate;
}

} // namespace pl::model
