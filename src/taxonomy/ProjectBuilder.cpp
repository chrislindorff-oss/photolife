#include "taxonomy/ProjectBuilder.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace pl::taxonomy {
namespace {
constexpr int kBatchSize = 30;      // iNat allows up to 30 ids per /taxa call
constexpr int kFirstConfirmAt = 10000;   // ask the user before going past this
constexpr int kConfirmEvery = 5000;      // then re-ask every this many species

// Every write to a shared Postgres catalogue is a network round trip, unlike
// local SQLite where a whole page finishes before the OS would ever notice.
// Yielding to the event loop every few writes (paint/timer events only --
// ExcludeUserInputEvents avoids the user re-entrantly clicking something
// mid-batch) keeps the window responsive and lets the progress text below
// actually get painted, instead of the app looking hung for the whole page.
constexpr int kYieldEvery = 10;

void maybeYield(int count)
{
    if (count % kYieldEvery == 0)
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

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
    m_startPage = 1;
    m_speciesTotal = 0;
    m_speciesSeen = 0;
    m_nextConfirmAt = kFirstConfirmAt;
    m_pendingPage = 0;
    m_awaitingConfirm = false;
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
    if (m_request.confirmedPlace) {
        m_store.upsertPlace(*m_request.confirmedPlace);
        m_placeId = m_request.confirmedPlace->inatId;
        resolveRootTaxon();
        return;
    }

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

    if (m_request.confirmedTaxon) {
        useRootTaxon(*m_request.confirmedTaxon);
        return;
    }

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

        useRootTaxon(*chosen);
    });
}

void ProjectBuilder::useRootTaxon(const Taxon &taxon)
{
    m_rootTaxonId = taxon.inatId;
    m_store.upsertTaxon(taxon);
    m_stored.insert(m_rootTaxonId);

    m_projectId = m_store.ensureProject(
        m_request.projectName, m_rootTaxonId,
        m_placeId > 0 ? std::optional<qint64>(m_placeId) : std::nullopt,
        QStringLiteral("inat"));
    if (m_projectId < 0) {
        fail(tr("could not create the project"));
        return;
    }

    // Loaded once per run (not per taxon, to avoid a query per row inside the
    // batches below): taxa deliberately pruned from this project earlier,
    // which a refresh must never resurrect. Empty for a brand-new project.
    const QList<qint64> excluded = m_store.projectExcludedTaxonIds(m_projectId);
    m_excludedTaxonIds = QSet<qint64>(excluded.begin(), excluded.end());

    if (!m_excludedTaxonIds.contains(m_rootTaxonId))
        m_store.addProjectTaxon(m_projectId, m_rootTaxonId, false, false);

    // Resume an interrupted run's species pagination if an earlier run left a
    // checkpoint whose resolved parameters match this one -- otherwise a
    // stale checkpoint (e.g. from a build against a different place) is
    // discarded rather than trusted. Back up one page: iNaturalist's
    // species_counts pagination isn't guaranteed stable across separate
    // calls, so resuming exactly at nextPage risks silently skipping a
    // handful of species at the boundary; every write is an idempotent
    // upsert, so redoing one page is cheap insurance.
    if (const auto checkpoint = m_store.buildCheckpoint(m_projectId)) {
        const bool sameParams = checkpoint->rootTaxonInatId == m_rootTaxonId
            && checkpoint->placeInatId == (m_placeId > 0 ? std::optional<qint64>(m_placeId)
                                                          : std::nullopt)
            && checkpoint->perPage == m_request.perPage;
        if (sameParams) {
            m_startPage = std::max(1, checkpoint->nextPage - 1);
            // checkpoint->speciesSeen was the cumulative count AFTER the
            // backed-up page finished; since that page is about to be
            // re-fetched and re-counted, subtract it back out first so the
            // seen-vs-total check below doesn't overcount and stop a whole
            // page short of the real total. (A no-op adjustment when
            // resuming at page 1, since there is nothing before it.)
            m_speciesSeen = m_startPage > 1
                ? std::max(0, checkpoint->speciesSeen - checkpoint->perPage)
                : 0;
            m_speciesTotal = checkpoint->speciesTotal;
            emit progress(tr("Resuming an interrupted build…"), m_speciesSeen, m_speciesTotal);
        } else {
            m_store.clearBuildCheckpoint(m_projectId);
        }
    }

    fetchRootDetail();
}

void ProjectBuilder::fetchRootDetail()
{
    if (checkCancelled())
        return;
    m_inat.fetchTaxon(m_rootTaxonId, [this](pl::net::Outcome<pl::net::TaxonDetail> out) {
        if (checkCancelled())
            return;
        if (out.ok()) {
            // The spine above the root (kingdom .. order), plus the root
            // taxon's own detail -- upserted together in one call, but only
            // the spine gets a project_taxon link (useRootTaxon() already
            // linked the root itself before this ever runs).
            QList<Taxon> toUpsert;
            for (const Taxon &a : out.value.ancestors) {
                if (!m_excludedTaxonIds.contains(a.inatId))
                    toUpsert.append(a);
            }
            toUpsert.append(out.value.taxon);

            bool ok = m_store.beginBatch();
            QHash<qint64, int> localIds;
            if (ok)
                localIds = m_store.upsertTaxa(toUpsert);
            ok = ok && localIds.size() == toUpsert.size();
            if (ok) {
                QList<TaxonomyStore::ProjectTaxonLink> links;
                for (const Taxon &a : out.value.ancestors) {
                    if (m_excludedTaxonIds.contains(a.inatId))
                        continue;
                    m_stored.insert(a.inatId);
                    links.append({localIds.value(a.inatId), false, false});
                }
                ok = m_store.addProjectTaxa(m_projectId, links);
            }
            if (ok)
                ok = m_store.commitBatch();
            if (!ok) {
                m_store.rollbackBatch();
                fail(tr("could not save the reference tree"));
                return;
            }
        }
        fetchSpeciesPage(m_startPage);
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

        const int pageCount = int(out.value.results.size());
        int written = 0;
        QList<Taxon> toUpsert;
        for (const pl::net::SpeciesCount &sc : out.value.results) {
            // Skip a species that was deliberately pruned, or that sits under
            // a pruned group -- iNaturalist has no notion of our exclusions,
            // so a group prune must keep excluding whatever it finds under
            // that group on every future refresh, not just what existed when
            // it was pruned.
            const bool excluded = m_excludedTaxonIds.contains(sc.taxon.inatId)
                || std::any_of(sc.ancestorIds.begin(), sc.ancestorIds.end(),
                               [this](qint64 a) { return m_excludedTaxonIds.contains(a); });
            if (!excluded) {
                toUpsert.append(sc.taxon);
                for (qint64 anc : sc.ancestorIds)
                    m_neededAncestors.insert(anc);
            }
            ++m_speciesSeen;
            ++written;
            if (written % kYieldEvery == 0 || written == pageCount) {
                emit progress(tr("Uploading to catalogue (%1 of %2 total resolved)")
                                  .arg(m_speciesSeen)
                                  .arg(m_speciesTotal),
                              written, pageCount);
            }
            maybeYield(written);
        }

        // The whole page's writes collapse into a small, constant number of
        // round trips (one multi-row upsert of the taxa, one of the
        // project_taxon links) instead of ~5 PER species -- the difference
        // between a build taking seconds and minutes against a remote
        // Postgres catalogue.
        bool ok = m_store.beginBatch();
        QHash<qint64, int> localIds;
        if (ok && !toUpsert.isEmpty())
            localIds = m_store.upsertTaxa(toUpsert);
        ok = ok && (toUpsert.isEmpty() || localIds.size() == toUpsert.size());
        if (ok && !localIds.isEmpty()) {
            QList<TaxonomyStore::ProjectTaxonLink> links;
            for (auto it = localIds.constBegin(); it != localIds.constEnd(); ++it) {
                m_stored.insert(it.key());
                links.append({it.value(), true, false});
            }
            ok = m_store.addProjectTaxa(m_projectId, links);
        }
        if (ok) {
            BuildCheckpoint checkpoint;
            checkpoint.rootTaxonInatId = m_rootTaxonId;
            checkpoint.placeInatId =
                m_placeId > 0 ? std::optional<qint64>(m_placeId) : std::nullopt;
            checkpoint.perPage = m_request.perPage;
            checkpoint.nextPage = page + 1;
            checkpoint.speciesSeen = m_speciesSeen;
            checkpoint.speciesTotal = m_speciesTotal;
            ok = m_store.saveBuildCheckpoint(m_projectId, checkpoint);
        }
        if (ok)
            ok = m_store.commitBatch();
        if (!ok) {
            m_store.rollbackBatch();
            fail(tr("could not save species to the catalogue"));
            return;
        }

        emit progress(tr("Fetching species"), m_speciesSeen, m_speciesTotal);

        const bool more = !out.value.results.isEmpty() && m_speciesSeen < m_speciesTotal;
        if (!more) {
            fillAncestors();
            return;
        }

        // Large trees mean thousands of API requests, so check in with the user
        // at 10,000 species and every 1,000 after that before carrying on.
        if (m_speciesSeen >= m_nextConfirmAt) {
            m_awaitingConfirm = true;
            m_pendingPage = page + 1;
            emit progress(tr("Waiting to continue"), m_speciesSeen, m_speciesTotal);
            emit confirmMoreSpecies(m_speciesSeen, m_speciesTotal);
            return;
        }

        fetchSpeciesPage(page + 1);
    });
}

void ProjectBuilder::continueFetching()
{
    if (!m_awaitingConfirm)
        return;
    m_awaitingConfirm = false;
    m_nextConfirmAt += kConfirmEvery;
    fetchSpeciesPage(m_pendingPage);
}

void ProjectBuilder::stopFetching()
{
    if (!m_awaitingConfirm)
        return;
    m_awaitingConfirm = false;
    // The user chose to stop here — this is a normal finish, not a failure.
    // Fill the intermediate ranks so the partial tree is still connected.
    fillAncestors();
}

void ProjectBuilder::fillAncestors()
{
    if (checkCancelled())
        return;

    // Species pagination is done, whether exhausted or the user stopped via
    // stopFetching() -- clear the checkpoint now rather than only on overall
    // success, so a build that fails partway through the (cheaper, already
    // self-resuming) ancestor fill doesn't cause the next run to re-walk
    // species pages it already finished.
    m_store.clearBuildCheckpoint(m_projectId);

    // A resumed run's in-memory m_neededAncestors only reflects species
    // fetched THIS run -- species committed by an earlier, interrupted run
    // never populate it, so their ancestors would otherwise silently go
    // unfilled. Folding this in unconditionally (not just when resumed)
    // keeps this function correct-by-construction.
    for (qint64 id : m_store.projectSpeciesAncestorIds(m_projectId))
        m_neededAncestors.insert(id);

    QList<qint64> pending;
    for (qint64 id : m_neededAncestors) {
        if (id <= 0 || m_stored.contains(id) || m_excludedTaxonIds.contains(id))
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
        QList<Taxon> toUpsert;
        for (const Taxon &t : out.value) {
            if (!m_excludedTaxonIds.contains(t.inatId))
                toUpsert.append(t);
        }

        bool ok = m_store.beginBatch();
        QHash<qint64, int> localIds;
        if (ok && !toUpsert.isEmpty())
            localIds = m_store.upsertTaxa(toUpsert);
        ok = ok && (toUpsert.isEmpty() || localIds.size() == toUpsert.size());
        if (ok && !localIds.isEmpty()) {
            QList<TaxonomyStore::ProjectTaxonLink> links;
            for (auto it = localIds.constBegin(); it != localIds.constEnd(); ++it)
                links.append({it.value(), false, false});
            ok = m_store.addProjectTaxa(m_projectId, links);
        }
        if (ok) {
            for (qint64 inatId : localIds.keys()) {
                m_stored.insert(inatId);
                ++m_ancestorFilled;
            }
            ok = m_store.commitBatch();
        }
        if (!ok) {
            m_store.rollbackBatch();
            fail(tr("could not save the reference tree"));
            return;
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
