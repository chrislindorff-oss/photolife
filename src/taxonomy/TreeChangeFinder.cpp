#include "taxonomy/TreeChangeFinder.h"

#include "net/INatClient.h"

#include <algorithm>
#include <QPointer>

namespace pl::taxonomy {

TreeChangeFinder::TreeChangeFinder(net::INatClient &inat, TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void TreeChangeFinder::find(int projectId, const QSet<qint64> &listedTaxa)
{
    if (m_running)
        return;
    m_running = true;
    m_projectId = projectId;
    m_fetched.clear();
    m_fetchingReplacements = false;
    m_candidates = m_store.projectChangeCandidates(projectId, listedTaxa);
    m_toFetch.clear();
    for (const TaxonomyStore::ChangeCandidate &c : std::as_const(m_candidates))
        m_toFetch << c.inatId;
    fetchNext();
}

void TreeChangeFinder::fetchNext()
{
    if (m_toFetch.isEmpty()) {
        if (!m_fetchingReplacements)
            fetchReplacements();
        else
            classify();
        return;
    }
    const QList<qint64> batch = m_toFetch.mid(0, kBatch);
    m_toFetch.remove(0, batch.size());
    m_inat.fetchTaxa(batch, [this, self = QPointer<TreeChangeFinder>(this)](net::Outcome<QList<Taxon>> out) {
        if (!self)
            return;   // gone while the request was out
        if (!out.ok()) {
            done(out.error);
            return;
        }
        for (const Taxon &t : std::as_const(out.value))
            m_fetched.insert(t.inatId, t);
        fetchNext();
    });
}

void TreeChangeFinder::fetchReplacements()
{
    m_fetchingReplacements = true;
    QSet<qint64> wanted;
    for (const Taxon &t : std::as_const(m_fetched)) {
        if (t.isActive)
            continue;
        for (qint64 id : t.currentSynonymIds) {
            if (!m_fetched.contains(id))
                wanted.insert(id);
        }
    }
    m_toFetch = QList<qint64>(wanted.cbegin(), wanted.cend());
    std::sort(m_toFetch.begin(), m_toFetch.end());
    fetchNext();
}

void TreeChangeFinder::classify()
{
    QList<TreeChange> changes;
    for (const TaxonomyStore::ChangeCandidate &c : std::as_const(m_candidates)) {
        TreeChange change;
        change.taxon = c;
        const auto it = m_fetched.constFind(c.inatId);
        // Gone from iNaturalist altogether, or inactive: renamed, split or retired.
        if (it == m_fetched.constEnd() || !it->isActive) {
            const QList<qint64> current = it == m_fetched.constEnd() ? QList<qint64>{} : it->currentSynonymIds;
            for (qint64 id : current) {
                if (m_fetched.contains(id))
                    change.replacements << m_fetched.value(id);
            }
            change.kind = change.replacements.size() == 1 ? TreeChange::Renamed
                          : change.replacements.isEmpty() ? TreeChange::Retired
                                                          : TreeChange::Split;
            changes << change;
            continue;
        }
        if (c.kept)
            continue;
        if (c.emptyGroup) {
            change.kind = TreeChange::EmptyGroup;
            changes << change;
        } else if (c.fromChecklist) {
            continue;
        } else if (c.rank == QLatin1String("species")) {
            if (c.inRegion) {
                change.kind = TreeChange::NotRecorded;
                changes << change;
            }
        } else {
            change.kind = TreeChange::SubspeciesNotRecorded;
            changes << change;
        }
    }
    sort(changes);
    m_running = false;
    emit finished(m_projectId, changes, QString());
}

void TreeChangeFinder::sort(QList<TreeChange> &changes)
{
    std::stable_sort(changes.begin(), changes.end(), [](const TreeChange &a, const TreeChange &b) {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return a.taxon.name.compare(b.taxon.name, Qt::CaseInsensitive) < 0;
    });
}

void TreeChangeFinder::done(const QString &error)
{
    m_running = false;
    emit finished(m_projectId, {}, error);
}

} // namespace pl::taxonomy
