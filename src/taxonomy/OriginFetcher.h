#pragma once

#include <QList>
#include <QObject>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Fills in which of a reference tree's taxa iNaturalist lists as introduced
// in the tree's region: pages observations/species_counts?introduced=true for
// the tree's root taxon and place, records each as introduced there
// (TaxonomyStore::addIntroduced, source "iNaturalist") and stamps the tree's
// origins_fetched_at. Trees are worked through one at a time from a queue, in
// the background; a failed fetch leaves the tree unstamped, so it's retried
// next launch.
//
// iNaturalist only knows taxa with observations in the region, so a
// checklist's origins (which win over these) fill the gaps.
//
// Drive it from the thread that owns the INatClient / store connection.
class OriginFetcher : public QObject
{
    Q_OBJECT

public:
    OriginFetcher(net::INatClient &inat, TaxonomyStore &store, QObject *parent = nullptr);

    // Queues trees (duplicates of queued or running ones are ignored).
    void enqueue(const QList<int> &projectIds);
    bool isRunning() const { return m_currentProject > 0; }

    static constexpr int kPerPage = 500;

signals:
    // `written` origins recorded, or ok = false when the fetch failed.
    void projectFinished(int projectId, bool ok, int written);

private:
    void startNext();
    void fetchPage();
    void finishProject(bool ok);

    net::INatClient &m_inat;
    TaxonomyStore &m_store;
    QList<int> m_queue;
    int m_currentProject = 0;
    qint64 m_rootTaxon = 0;
    qint64 m_place = 0;
    int m_page = 1;
    int m_seen = 0;
    QList<qint64> m_introduced;
};

} // namespace pl::taxonomy
