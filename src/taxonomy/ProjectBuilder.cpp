#include "taxonomy/ProjectBuilder.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QtGlobal>

#include <cmath>

namespace pl::taxonomy {
namespace {
constexpr int kBatchSize = 30;   // iNat allows up to 30 ids per /taxa call
} // namespace

ProjectBuilder::ProjectBuilder(pl::net::INatClient &inat, TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void ProjectBuilder::start(const Request &request)
{
    if (m_running)
        return;
    m_request = request;
    m_running = true;
    m_cancelled = false;
    m_placeId = 0;
    m_rootTaxonId = 0;
    m_projectId = -1;
    m_speciesTotal = 0;
    m_speciesSeen = 0;
    m_stored.clear();
    m_neededAncestors.clear();
    m_ancestorBatches.clear();
    m_ancestorBatchIndex = 0;
    m_ancestorFilled = 0;
    m_ancestorTotal = 0;

    if (m_request.projectName.trimmed().isEmpty() || m_request.taxonQuery.trimmed().isEmpty()) {
        fail(QStringLiteral("a project name and a taxon are required"));
        return;
    }

    resolvePlace();
}

bool ProjectBuilder::checkCancelled()
{
    if (!m_running)
        return true;   // already finished or failed: swallow any late callbacks
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void ProjectBuilder::resolvePlace()
{
    if (m_request.placeQuery.trimmed().isEmpty()) {
        resolveRootTaxon();
        return;
    }

    emit progress(tr("Resolving region"), 0, 0);
    m_inat.resolvePlaces(m_request.placeQuery, [this](pl::net::Outcome<QList<Place>> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(tr("region lookup failed: %1").arg(out.error));
            return;
        }
        if (out.value.isEmpty()) {
            fail(tr("no region matched \"%1\"").arg(m_request.placeQuery));
            return;
        }

        // Prefer an exact display-name match, else the first result.
        Place chosen = out.value.first();
        for (const Place &p : out.value) {
            if (p.displayName.compare(m_request.placeQuery, Qt::CaseInsensitive) == 0
                || p.name.compare(m_request.placeQuery, Qt::CaseInsensitive) == 0) {
                chosen = p;
                break;
            }
        }

        m_store.upsertPlace(chosen);
        m_placeId = chosen.inatId;
        resolveRootTaxon();
    });
}

void ProjectBuilder::resolveRootTaxon()
{
    if (checkCancelled())
        return;
    emit progress(tr("Resolving taxon"), 0, 0);
    m_inat.searchTaxa(m_request.taxonQuery, m_request.rank,
                      [this](pl::net::Outcome<QList<Taxon>> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(tr("taxon lookup failed: %1").arg(out.error));
            return;
        }

        const Taxon *chosen = nullptr;
        for (const Taxon &t : out.value) {
            if (t.isActive) {
                chosen = &t;
                break;
            }
        }
        if (!chosen && !out.value.isEmpty())
            chosen = &out.value.first();
        if (!chosen) {
            fail(tr("no taxon matched \"%1\"").arg(m_request.taxonQuery));
            return;
        }

        m_rootTaxonId = chosen->inatId;
        m_store.upsertTaxon(*chosen);
        m_stored.insert(m_rootTaxonId);

        m_projectId = m_store.ensureProject(
            m_request.projectName, m_rootTaxonId,
            m_placeId > 0 ? std::optional<qint64>(m_placeId) : std::nullopt,
            QStringLiteral("inat"));
        if (m_projectId < 0) {
            fail(tr("could not create the project"));
            return;
        }
        m_store.addProjectTaxon(m_projectId, m_rootTaxonId, false, false);

        fetchRootDetail();
    });
}

void ProjectBuilder::fetchRootDetail()
{
    if (checkCancelled())
        return;
    m_inat.fetchTaxon(m_rootTaxonId, [this](pl::net::Outcome<pl::net::TaxonDetail> out) {
        if (checkCancelled())
            return;
        if (out.ok()) {
            // The spine above the root (kingdom .. order) and its immediate children.
            for (const Taxon &a : out.value.ancestors) {
                m_store.upsertTaxon(a);
                m_stored.insert(a.inatId);
                m_store.addProjectTaxon(m_projectId, a.inatId, false, false);
            }
            m_store.upsertTaxon(out.value.taxon);
        }
        fetchSpeciesPage(1);
    });
}

void ProjectBuilder::fetchSpeciesPage(int page)
{
    if (checkCancelled())
        return;
    emit progress(tr("Fetching species"), m_speciesSeen, m_speciesTotal);

    m_inat.speciesCounts(m_rootTaxonId, m_placeId, page, m_request.perPage,
                         [this, page](pl::net::Outcome<pl::net::SpeciesCountsPage> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(tr("species list failed: %1").arg(out.error));
            return;
        }

        if (page == 1)
            m_speciesTotal = out.value.totalResults;

        for (const pl::net::SpeciesCount &sc : out.value.results) {
            m_store.upsertTaxon(sc.taxon);
            m_stored.insert(sc.taxon.inatId);
            m_store.addProjectTaxon(m_projectId, sc.taxon.inatId, true, false);
            for (qint64 anc : sc.ancestorIds)
                m_neededAncestors.insert(anc);
            ++m_speciesSeen;
        }

        emit progress(tr("Fetching species"), m_speciesSeen, m_speciesTotal);

        const int perPage = qMax(1, out.value.perPage);
        const bool more = !out.value.results.isEmpty()
                          && m_speciesSeen < m_speciesTotal
                          && qint64(page) * perPage < 10000;
        if (more)
            fetchSpeciesPage(page + 1);
        else
            fillAncestors();
    });
}

void ProjectBuilder::fillAncestors()
{
    if (checkCancelled())
        return;

    QList<qint64> pending;
    for (qint64 id : m_neededAncestors) {
        if (id <= 0 || m_stored.contains(id))
            continue;
        if (m_store.taxonLocalId(id)) {
            // Already cached from a previous run; still link it into this project.
            m_store.addProjectTaxon(m_projectId, id, false, false);
            m_stored.insert(id);
            continue;
        }
        pending.append(id);
    }

    m_ancestorTotal = int(pending.size());
    for (int i = 0; i < pending.size(); i += kBatchSize)
        m_ancestorBatches.append(pending.mid(i, kBatchSize));
    m_ancestorBatchIndex = 0;
    m_ancestorFilled = 0;

    fillNextAncestorBatch();
}

void ProjectBuilder::fillNextAncestorBatch()
{
    if (checkCancelled())
        return;
    if (m_ancestorBatchIndex >= m_ancestorBatches.size()) {
        succeed();
        return;
    }

    const QList<qint64> batch = m_ancestorBatches.at(m_ancestorBatchIndex++);
    emit progress(tr("Filling intermediate taxa"), m_ancestorFilled, m_ancestorTotal);

    m_inat.fetchTaxa(batch, [this](pl::net::Outcome<QList<Taxon>> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(tr("filling the tree failed: %1").arg(out.error));
            return;
        }
        for (const Taxon &t : out.value) {
            m_store.upsertTaxon(t);
            m_store.addProjectTaxon(m_projectId, t.inatId, false, false);
            m_stored.insert(t.inatId);
            ++m_ancestorFilled;
        }
        emit progress(tr("Filling intermediate taxa"), m_ancestorFilled, m_ancestorTotal);
        fillNextAncestorBatch();
    });
}

void ProjectBuilder::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, m_projectId);
}

void ProjectBuilder::succeed()
{
    if (m_projectId >= 0)
        m_store.setProjectRefreshedNow(m_projectId);
    m_running = false;
    emit progress(tr("Done"), m_speciesSeen, m_speciesTotal);
    emit finished(true, QString(), m_projectId);
}

} // namespace pl::taxonomy
