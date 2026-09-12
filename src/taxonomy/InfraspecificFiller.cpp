#include "taxonomy/InfraspecificFiller.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

namespace pl::taxonomy {

InfraspecificFiller::InfraspecificFiller(pl::net::INatClient &inat, TaxonomyStore &store,
                                         QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void InfraspecificFiller::start(int projectId, qint64 scopeInatId)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_projectId = projectId;
    m_placeId = m_store.projectPlaceInatId(projectId).value_or(0);
    m_index = 0;
    m_added = 0;
    m_currentSpecies = 0;
    m_candidates.clear();
    m_candidateIndex = 0;
    m_pending = m_store.projectSpeciesNeedingInfraCheck(projectId, scopeInatId);

    emit progress(0, int(m_pending.size()));
    fetchNextSpecies();
}

bool InfraspecificFiller::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void InfraspecificFiller::fetchNextSpecies()
{
    if (checkCancelled())
        return;
    if (m_index >= m_pending.size()) {
        succeed();
        return;
    }

    const qint64 speciesId = m_pending.at(m_index);
    m_inat.fetchTaxon(speciesId, [this, speciesId](pl::net::Outcome<pl::net::TaxonDetail> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(out.error);
            return;
        }

        m_currentSpecies = speciesId;
        m_candidates.clear();
        for (const Taxon &child : out.value.children) {
            if (child.isActive && TaxonomyStore::isInfraspecificRank(child.rank))
                m_candidates.append(child);
        }
        m_candidateIndex = 0;
        checkNextCandidate();
    });
}

void InfraspecificFiller::checkNextCandidate()
{
    if (checkCancelled())
        return;

    if (m_candidateIndex >= m_candidates.size()) {
        finishCurrentSpecies();
        return;
    }

    const Taxon child = m_candidates.at(m_candidateIndex);

    const auto accept = [this, child] {
        m_store.upsertTaxon(child);
        if (m_store.addProjectTaxon(m_projectId, child.inatId, true, false))
            ++m_added;
        ++m_candidateIndex;
        checkNextCandidate();
    };
    const auto skip = [this] {
        ++m_candidateIndex;
        checkNextCandidate();
    };

    // No place filter: the whole world is "in region", so take every child.
    if (m_placeId <= 0) {
        accept();
        return;
    }

    m_inat.observationCount(child.inatId, m_placeId,
                            [this, accept, skip](pl::net::Outcome<int> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(out.error);
            return;
        }
        if (out.value > 0)
            accept();
        else
            skip();
    });
}

void InfraspecificFiller::finishCurrentSpecies()
{
    m_store.markInfraChecked(m_projectId, m_currentSpecies);
    ++m_index;
    emit progress(m_index, int(m_pending.size()));
    fetchNextSpecies();
}

void InfraspecificFiller::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, m_added);
}

void InfraspecificFiller::succeed()
{
    m_running = false;
    emit finished(true, QString(), m_added);
}

} // namespace pl::taxonomy
