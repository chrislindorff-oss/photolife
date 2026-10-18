#include "treefile/TreeImporter.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

namespace pl::treefile {
namespace {

constexpr int kBatchSize = 30;   // iNat's limit for /taxa/{ids} and /places/{ids}

using pl::taxonomy::TaxonomyStore;

QList<qint64> ancestryIds(const QString &ancestry)
{
    QList<qint64> ids;
    for (const QString &part : ancestry.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 id = part.toLongLong(&ok);
        if (ok && id > 0)
            ids.append(id);
    }
    return ids;
}

} // namespace

TreeImporter::TreeImporter(pl::net::INatClient &inat, TaxonomyStore &store, QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

bool TreeImporter::stop()
{
    if (!m_running)
        return true;
    if (m_cancelled) {
        fail(tr("cancelled"));
        return true;
    }
    return false;
}

void TreeImporter::fail(const QString &error)
{
    // A half-built tree is more confusing than none: the file is still there
    // to import again.
    if (m_projectId > 0)
        m_store.deleteProject(m_projectId);
    m_running = false;
    emit finished(false, error, -1, m_imported, m_lookedUp, m_unresolved);
}

void TreeImporter::start(const Request &request)
{
    if (m_running)
        return;
    m_request = request;
    m_running = true;
    m_cancelled = false;
    m_projectId = -1;
    m_nameRows.clear();
    m_idRows.clear();
    m_nameIndex = 0;
    m_linked.clear();
    m_wanted.clear();
    m_ancestryOf.clear();
    m_fillFrom.clear();
    m_batches.clear();
    m_batchIndex = 0;
    m_imported = m_lookedUp = m_unresolved = 0;

    const QString name = m_request.treeName.trimmed();
    if (name.isEmpty()) {
        fail(tr("the tree needs a name"));
        return;
    }
    if (m_store.projectIdByName(name)) {
        fail(tr("a reference tree called \"%1\" already exists").arg(name));
        return;
    }
    const TreeFile &file = m_request.file;
    m_projectId = m_store.ensureProject(name, file.rootInatId, file.placeInatId,
                                        file.rootInatId ? QStringLiteral("inat")
                                                        : QStringLiteral("checklist"));
    if (m_projectId < 0) {
        fail(tr("could not create the reference tree"));
        return;
    }
    fetchPlace();
}

// The tree's place, when this catalogue hasn't seen it -- the locality
// restriction and the coverage panel need its name and bounding box. Not
// fatal: the tree still works without it.
void TreeImporter::fetchPlace()
{
    const auto place = m_request.file.placeInatId;
    auto next = [this] {
        if (stop())
            return;
        if (!writeKnownRows()) {
            fail(tr("could not write the taxa to the catalogue"));
            return;
        }
        resolveNext();
    };
    if (!place || m_store.placeByInatId(*place)) {
        next();
        return;
    }
    m_inat.fetchPlaces({*place}, [this, next](pl::net::Outcome<QList<pl::taxonomy::Place>> out) {
        if (out.ok()) {
            for (const auto &p : out.value)
                m_store.upsertPlace(p);
        }
        next();
    });
}

void TreeImporter::linkRows(const QList<PendingLink> &links)
{
    if (links.isEmpty())
        return;
    QList<qint64> ids;
    for (const PendingLink &l : links)
        ids << l.inatId;
    const QHash<qint64, pl::taxonomy::Taxon> cached = m_store.taxaByInatIds(ids);
    const QHash<qint64, qint64> local = m_store.taxonLocalIds(ids);

    const TreeFile &file = m_request.file;
    QList<TaxonomyStore::ProjectTaxonLink> projectLinks;
    QList<std::pair<qint64, pl::taxonomy::StatusRecord>> statuses;
    QList<std::pair<qint64, pl::taxonomy::OriginRecord>> origins;
    QList<const PendingLink *> linked;
    for (const PendingLink &l : links) {
        const auto localId = local.constFind(l.inatId);
        if (localId == local.constEnd())
            continue;
        const TreeFileRow &row = file.rows.at(l.row);
        const pl::taxonomy::Taxon taxon = cached.value(l.inatId);
        const QString rank = taxon.rank.isEmpty() ? row.rank : taxon.rank;
        projectLinks.append({int(*localId), row.inRegion.value_or(TaxonomyStore::isLeafRank(rank)),
                             l.fromChecklist});
        m_ancestryOf.insert(l.inatId, taxon.ancestry.isEmpty() ? row.ancestry : taxon.ancestry);
        linked.append(&l);
        // The file's common name, so photos filed under it match too (kept
        // when iNaturalist's names are refreshed).
        if (!row.commonName.trimmed().isEmpty())
            m_store.addName(l.inatId, row.commonName.trimmed(), TaxonomyStore::kUserVernacular);

        for (const TreeFileFact &f : row.statuses) {
            pl::taxonomy::StatusRecord s;
            s.status = f.value;
            s.source = f.source.isEmpty() ? m_request.defaultSource : f.source;
            s.placeInatId = f.placeInatId ? f.placeInatId : file.placeInatId;
            statuses.append({l.inatId, s});
        }
        for (const TreeFileFact &f : row.origins) {
            pl::taxonomy::OriginRecord o;
            o.origin = f.value;
            o.source = f.source.isEmpty() ? m_request.defaultSource : f.source;
            o.placeInatId = f.placeInatId ? f.placeInatId : file.placeInatId;
            origins.append({l.inatId, o});
        }
    }
    if (!m_store.addProjectTaxa(m_projectId, projectLinks))
        return;
    for (const PendingLink *l : std::as_const(linked)) {
        if (!m_linked.contains(l->inatId))
            ++m_imported;
        m_linked.insert(l->inatId);
        if (l->fillAncestors)
            m_fillFrom.insert(l->inatId);
    }
    m_store.addStatuses(statuses);
    m_store.addOrigins(origins);
}

// Every row whose taxon can be written without the network, in one
// transaction: the file's own taxa (new to this catalogue) and any already
// cached. Sorts the rest into name lookups and id fetches.
bool TreeImporter::writeKnownRows()
{
    const QList<TreeFileRow> &rows = m_request.file.rows;

    QList<qint64> fileIds;
    for (const TreeFileRow &row : rows) {
        if (row.inatId)
            fileIds << *row.inatId;
    }
    const QHash<qint64, qint64> alreadyCached = m_store.taxonLocalIds(fileIds);

    QList<pl::taxonomy::Taxon> fresh;
    QList<int> known;
    for (int i = 0; i < rows.size(); ++i) {
        const TreeFileRow &row = rows.at(i);
        if (!row.inatId) {
            m_nameRows.append(i);
            continue;
        }
        if (alreadyCached.contains(*row.inatId)) {
            known.append(i);   // the cache's own record wins
            continue;
        }
        if (row.name.isEmpty() || row.rank.isEmpty()) {
            m_idRows.append(i);   // not enough in the file to write it
            continue;
        }
        pl::taxonomy::Taxon t;
        t.inatId = *row.inatId;
        t.parentInatId = row.parentInatId;
        t.rank = row.rank;
        t.rankLevel = row.rankLevel;
        t.name = row.name;
        t.commonName = row.commonName;
        t.ancestry = row.ancestry;
        t.isActive = row.isActive;
        fresh.append(t);
        known.append(i);
    }

    if (!m_store.beginBatch())
        return false;
    if (!fresh.isEmpty() && m_store.upsertTaxa(fresh).isEmpty()) {
        m_store.rollbackBatch();
        return false;
    }
    const bool handMade = m_request.file.formatVersion == 0;
    QList<PendingLink> links;
    for (int i : known)
        links.append({i, *rows.at(i).inatId, rows.at(i).fromChecklist, handMade});
    linkRows(links);
    return m_store.commitBatch();
}

void TreeImporter::resolveNext()
{
    const QList<TreeFileRow> &rows = m_request.file.rows;
    // Rows the cache can answer are handled in a loop; only return to the
    // event loop while an iNaturalist search is in flight.
    while (true) {
        if (stop())
            return;
        if (m_nameIndex >= m_nameRows.size()) {
            fetchMissing();
            return;
        }
        emit progress(m_nameIndex, int(m_nameRows.size()));
        const int rowIndex = m_nameRows.at(m_nameIndex++);
        const TreeFileRow &row = rows.at(rowIndex);
        const QString folded = TaxonomyStore::foldName(row.name);

        if (const auto cached = m_store.taxonInatIdByFoldedName(folded)) {
            linkRows({{rowIndex, *cached, true, true}});
            continue;
        }

        m_inat.searchTaxa(row.name, row.rank,
            [this, rowIndex, folded](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
                const TreeFileRow &row = m_request.file.rows.at(rowIndex);
                if (stop())
                    return;
                if (!out.ok()) {
                    fail(tr("lookup failed for \"%1\": %2").arg(row.name, out.error));
                    return;
                }
                // An exact name match first, then any active taxon.
                const pl::taxonomy::Taxon *chosen = nullptr;
                for (const auto &t : out.value) {
                    if (t.isActive && TaxonomyStore::foldName(t.name) == folded) {
                        chosen = &t;
                        break;
                    }
                }
                for (const auto &t : out.value) {
                    if (!chosen && t.isActive)
                        chosen = &t;
                }
                if (!chosen && !out.value.isEmpty())
                    chosen = &out.value.first();

                if (!chosen) {
                    ++m_unresolved;
                } else {
                    m_store.upsertTaxon(*chosen);
                    // The file's spelling, so the matcher recognises it too.
                    if (TaxonomyStore::foldName(chosen->name) != folded)
                        m_store.addName(chosen->inatId, row.name, QStringLiteral("synonym"));
                    ++m_lookedUp;
                    linkRows({{rowIndex, chosen->inatId, true, true}});
                }
                resolveNext();
            });
        return;
    }
}

// Links what the cache now has (id-only rows, ancestors of every linked
// taxon), then fetches whatever is still missing -- ancestors, id-only rows,
// excluded taxa -- and goes round again, since a fetched id-only row brings
// ancestors of its own. Each id is asked for once.
void TreeImporter::fetchMissing()
{
    if (stop())
        return;
    const QList<TreeFileRow> &rows = m_request.file.rows;

    QSet<qint64> wanted;
    QList<qint64> idRowIds;
    for (int i : std::as_const(m_idRows)) {
        if (!m_linked.contains(*rows.at(i).inatId))
            idRowIds << *rows.at(i).inatId;
    }
    const QHash<qint64, qint64> idRowsCached = m_store.taxonLocalIds(idRowIds);
    QList<PendingLink> links;
    for (int i : std::as_const(m_idRows)) {
        const qint64 id = *rows.at(i).inatId;
        if (m_linked.contains(id))
            continue;
        if (idRowsCached.contains(id))
            links.append({i, id, rows.at(i).fromChecklist, true});
        else
            wanted.insert(id);
    }
    linkRows(links);

    // Ancestors the tree doesn't hold yet: the cached ones linked in one go,
    // the rest fetched.
    QSet<qint64> ancestorSet;
    for (qint64 taxon : std::as_const(m_fillFrom)) {
        for (qint64 id : ancestryIds(m_ancestryOf.value(taxon))) {
            if (!m_linked.contains(id))
                ancestorSet.insert(id);
        }
    }
    const QList<qint64> ancestors(ancestorSet.cbegin(), ancestorSet.cend());
    const QHash<qint64, qint64> ancestorsCached = m_store.taxonLocalIds(ancestors);
    QList<TaxonomyStore::ProjectTaxonLink> ancestorLinks;
    for (qint64 id : ancestors) {
        if (const auto it = ancestorsCached.constFind(id); it != ancestorsCached.constEnd())
            ancestorLinks.append({int(*it), false, false});
        else
            wanted.insert(id);
    }
    if (m_store.addProjectTaxa(m_projectId, ancestorLinks)) {
        for (auto it = ancestorsCached.cbegin(); it != ancestorsCached.cend(); ++it)
            m_linked.insert(it.key());
    }

    const QHash<qint64, qint64> excludedCached =
        m_store.taxonLocalIds(m_request.file.excludedInatIds);
    for (qint64 id : m_request.file.excludedInatIds) {
        if (!excludedCached.contains(id))
            wanted.insert(id);
    }

    wanted -= m_wanted;   // already asked for
    if (wanted.isEmpty()) {
        finishTree();
        return;
    }
    m_wanted += wanted;
    const QList<qint64> ids(wanted.cbegin(), wanted.cend());
    m_batches.clear();
    for (qsizetype i = 0; i < ids.size(); i += kBatchSize)
        m_batches.append(ids.mid(i, kBatchSize));
    m_batchIndex = 0;
    fetchNextBatch();
}

void TreeImporter::fetchNextBatch()
{
    if (stop())
        return;
    if (m_batchIndex >= m_batches.size()) {
        fetchMissing();
        return;
    }
    emit progress(m_batchIndex, int(m_batches.size()));
    const QList<qint64> batch = m_batches.at(m_batchIndex++);
    m_inat.fetchTaxa(batch, [this](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
        if (stop())
            return;
        // Not fatal: an ancestor that can't be fetched just leaves a gap.
        if (out.ok() && !out.value.isEmpty())
            m_store.upsertTaxa(out.value);
        fetchNextBatch();
    });
}

void TreeImporter::finishTree()
{
    const QList<TreeFileRow> &rows = m_request.file.rows;
    for (int i : std::as_const(m_idRows)) {
        if (!m_linked.contains(*rows.at(i).inatId))
            ++m_unresolved;
    }
    if (m_imported == 0) {
        fail(tr("none of the file's taxa could be found"));
        return;
    }

    m_store.addProjectExclusions(m_projectId, m_request.file.excludedInatIds);

    // No root in the file (a hand-made list): the deepest taxon every row
    // shares, so a later Refresh knows what the tree covers.
    const TreeFile &file = m_request.file;
    if (!file.rootInatId) {
        std::optional<QList<qint64>> common;
        for (auto it = m_ancestryOf.cbegin(); it != m_ancestryOf.cend(); ++it) {
            QList<qint64> chain = ancestryIds(it.value());
            chain.append(it.key());
            if (!common) {
                common = chain;
                continue;
            }
            qsizetype n = 0;
            while (n < common->size() && n < chain.size() && common->at(n) == chain.at(n))
                ++n;
            common->resize(n);
        }
        if (common && !common->isEmpty())
            m_store.ensureProject(m_request.treeName.trimmed(), common->last(), file.placeInatId,
                                  QStringLiteral("checklist"));
    }

    m_store.setProjectRefreshedNow(m_projectId);
    m_running = false;
    emit progress(1, 1);
    emit finished(true, QString(), m_projectId, m_imported, m_lookedUp, m_unresolved);
}

} // namespace pl::treefile
