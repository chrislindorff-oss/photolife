#include "taxonomy/OriginFetcher.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

namespace pl::taxonomy {

namespace {
const QString kSource = QStringLiteral("iNaturalist");
}

OriginFetcher::OriginFetcher(net::INatClient &inat, TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void OriginFetcher::enqueue(const QList<int> &projectIds)
{
    for (int id : projectIds) {
        if (id > 0 && id != m_currentProject && !m_queue.contains(id))
            m_queue.append(id);
    }
    if (!isRunning())
        startNext();
}

void OriginFetcher::startNext()
{
    while (!m_queue.isEmpty()) {
        const int pid = m_queue.takeFirst();
        const auto root = m_store.projectRootTaxonInatId(pid);
        const auto place = m_store.projectPlaceInatId(pid);
        if (!root || !place)
            continue;   // no region (or root) to ask about
        m_currentProject = pid;
        m_rootTaxon = *root;
        m_place = *place;
        m_page = 1;
        m_seen = 0;
        m_introduced.clear();
        fetchPage();
        return;
    }
}

void OriginFetcher::fetchPage()
{
    m_inat.introducedSpeciesCounts(
        m_rootTaxon, m_place, m_page, kPerPage, [this](net::Outcome<net::SpeciesCountsPage> out) {
            if (!out.ok()) {
                finishProject(false);
                return;
            }
            for (const net::SpeciesCount &sc : out.value.results)
                m_introduced.append(sc.taxon.inatId);
            m_seen += int(out.value.results.size());
            if (m_seen < out.value.totalResults && !out.value.results.isEmpty()) {
                ++m_page;
                fetchPage();
                return;
            }
            finishProject(true);
        });
}

void OriginFetcher::finishProject(bool ok)
{
    const int pid = m_currentProject;
    int written = 0;
    if (ok) {
        written = m_store.addIntroduced(m_place, kSource, m_introduced);
        if (written < 0)
            ok = false;
        else
            m_store.setProjectOriginsFetchedNow(pid);
    }
    m_currentProject = 0;
    emit projectFinished(pid, ok, ok ? written : 0);
    startNext();
}

} // namespace pl::taxonomy
