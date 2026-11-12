#include "taxonomy/TreeGapFiller.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"
#include <QPointer>

namespace pl::taxonomy {

TreeGapFiller::TreeGapFiller(net::INatClient &inat, TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void TreeGapFiller::fill(int projectId)
{
    if (m_running || projectId <= 0)
        return;
    m_running = true;
    m_projectId = projectId;
    m_pass = 0;
    m_added = 0;
    m_tried.clear();
    nextPass();
}

void TreeGapFiller::nextPass()
{
    // A level just added can lack its own parent in turn: look again, a few times at most.
    if (++m_pass > kMaxPasses) {
        done({});
        return;
    }
    m_toFetch.clear();
    int linked = 0;
    for (qint64 id : m_store.projectMissingAncestorIds(m_projectId)) {
        if (m_tried.contains(id))
            continue;
        m_tried.insert(id);
        // Already known from another tree: just link it in.
        if (m_store.taxonLocalId(id)) {
            if (m_store.addProjectTaxon(m_projectId, id, false, false))
                ++linked;
            continue;
        }
        m_toFetch << id;
    }
    m_added += linked;
    if (!m_toFetch.isEmpty())
        fetchNext();
    else if (linked > 0)
        nextPass();   // what was linked may name a level that's missing too
    else
        done({});
}

void TreeGapFiller::fetchNext()
{
    if (m_toFetch.isEmpty()) {
        nextPass();
        return;
    }
    const QList<qint64> batch = m_toFetch.mid(0, kBatch);
    m_toFetch.remove(0, batch.size());
    m_inat.fetchTaxa(batch, [this, self = QPointer<TreeGapFiller>(this)](net::Outcome<QList<Taxon>> out) {
        if (!self)
            return;   // gone while the request was out
        if (!out.ok()) {
            done(out.error);
            return;
        }
        bool ok = m_store.beginBatch();
        QHash<qint64, int> localIds;
        if (ok && !out.value.isEmpty())
            localIds = m_store.upsertTaxa(out.value);
        if (ok && !localIds.isEmpty()) {
            QList<TaxonomyStore::ProjectTaxonLink> links;
            for (auto it = localIds.constBegin(); it != localIds.constEnd(); ++it)
                links.append({it.value(), false, false});
            ok = m_store.addProjectTaxa(m_projectId, links);
        }
        if (ok)
            ok = m_store.commitBatch();
        if (!ok) {
            m_store.rollbackBatch();
            done(tr("couldn't save the taxa"));
            return;
        }
        m_added += int(localIds.size());
        fetchNext();
    });
}

void TreeGapFiller::done(const QString &error)
{
    m_running = false;
    emit finished(m_projectId, m_added, error);
}

} // namespace pl::taxonomy
