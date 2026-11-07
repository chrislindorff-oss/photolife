#pragma once

#include <QObject>
#include <QSet>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Mends a reference tree's gaps: levels iNaturalist added after the tree was
// built (a complex, a section) that a taxon's record already names as its
// parent, so the tree lacked them -- Hedera helix under the "Hedera helix"
// complex, say. Fetches just those taxa (TaxonomyStore::projectMissingAncestorIds)
// and links them into the tree; a full Refresh Tree isn't needed. Pruned
// branches are left alone.
//
// Drive it from the thread that owns the INatClient / store connection.
class TreeGapFiller : public QObject
{
    Q_OBJECT

public:
    TreeGapFiller(net::INatClient &inat, TaxonomyStore &store, QObject *parent = nullptr);

    void fill(int projectId);
    bool isRunning() const { return m_running; }

signals:
    // `added` taxa linked into the tree (0 when it had no gaps).
    void finished(int projectId, int added, const QString &error);

private:
    void nextPass();
    void fetchNext();
    void done(const QString &error);

    static constexpr int kBatch = 30;   // iNaturalist's /taxa/<ids> limit
    static constexpr int kMaxPasses = 3;

    net::INatClient &m_inat;
    TaxonomyStore &m_store;
    bool m_running = false;
    int m_projectId = 0;
    int m_pass = 0;
    int m_added = 0;
    QList<qint64> m_toFetch;
    QSet<qint64> m_tried;   // never asked for twice in one run
};

} // namespace pl::taxonomy
