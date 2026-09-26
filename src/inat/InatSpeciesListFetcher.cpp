#include "inat/InatSpeciesListFetcher.h"

#include "net/INatParse.h"
#include "taxonomy/TaxonomyStore.h"

#include <QSet>

namespace pl::inat {

InatSpeciesListFetcher::InatSpeciesListFetcher(net::INatClient &inat,
                                               taxonomy::TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void InatSpeciesListFetcher::start(const Request &request)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_request = request;
    m_page = 1;
    m_total = 0;
    m_leaves.clear();
    m_known.clear();
    m_missing.clear();
    m_missingDone = 0;
    emit progress(tr("Fetching %1's taxa from iNaturalist…").arg(request.userLogin), 0, 0);
    fetchCountsPage();
}

bool InatSpeciesListFetcher::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void InatSpeciesListFetcher::fetchCountsPage()
{
    if (checkCancelled())
        return;
    m_inat.userSpeciesCounts(
        m_request.userLogin, m_request.taxonId, m_request.placeId, m_request.quality, m_page,
        kPerPage, [this](net::Outcome<net::SpeciesCountsPage> out) {
            if (checkCancelled())
                return;
            if (!out.ok()) {
                fail(out.error);
                return;
            }
            m_total = out.value.totalResults;
            m_leaves.append(out.value.results);
            emit progress(tr("Fetching %1's taxa from iNaturalist…").arg(m_request.userLogin),
                          int(m_leaves.size()), m_total);
            if (m_leaves.size() < m_total && !out.value.results.isEmpty()) {
                ++m_page;
                fetchCountsPage();
                return;
            }
            resolveAncestors();
        });
}

void InatSpeciesListFetcher::resolveAncestors()
{
    // The tree leaves first: their own cached rows carry the ancestry the
    // roll-up walks.
    m_known = m_store.taxaByInatIds(m_request.treeLeafIds);

    QSet<qint64> needed;
    for (const net::SpeciesCount &leaf : std::as_const(m_leaves)) {
        for (qint64 id : leaf.ancestorIds)
            needed.insert(id);
    }
    for (const taxonomy::Taxon &t : std::as_const(m_known)) {
        for (qint64 id : net::inat::ancestryIds(t.ancestry))
            needed.insert(id);
    }
    for (auto it = needed.begin(); it != needed.end();) {
        if (m_known.contains(*it))
            it = needed.erase(it);
        else
            ++it;
    }

    const QHash<qint64, taxonomy::Taxon> cached = m_store.taxaByInatIds(needed.values());
    for (auto it = cached.cbegin(); it != cached.cend(); ++it) {
        m_known.insert(it.key(), it.value());
        needed.remove(it.key());
    }

    m_missing = needed.values();
    m_missingDone = 0;
    fetchNextTaxaBatch();
}

void InatSpeciesListFetcher::fetchNextTaxaBatch()
{
    if (checkCancelled())
        return;
    if (m_missingDone >= m_missing.size()) {
        m_running = false;
        emit finished(true, QString(), m_leaves, m_known);
        return;
    }
    emit progress(tr("Looking up the ranks of %n higher taxa…", nullptr, int(m_missing.size())),
                  m_missingDone, int(m_missing.size()));

    const QList<qint64> batch = m_missing.mid(m_missingDone, kTaxaBatchSize);
    m_inat.fetchTaxa(batch, [this, batch](net::Outcome<QList<taxonomy::Taxon>> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(out.error);
            return;
        }
        // Only ids that weren't cached at all get written, so this never
        // replaces a richer cached row (names, photo) with a leaner one.
        m_store.upsertTaxa(out.value);
        for (const taxonomy::Taxon &t : std::as_const(out.value))
            m_known.insert(t.inatId, t);
        m_missingDone += int(batch.size());
        fetchNextTaxaBatch();
    });
}

void InatSpeciesListFetcher::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, {}, {});
}

} // namespace pl::inat
