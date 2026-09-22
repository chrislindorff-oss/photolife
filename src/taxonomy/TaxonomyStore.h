#pragma once

#include "geo/GeoBox.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace pl::taxonomy {

// Read/write access to the taxonomy cache (schema v2): places, taxa and their
// names, reference-tree projects, and the API client's conditional-GET cache.
// Constructed with the name of an open QSqlDatabase connection; all calls run on
// the caller's thread.
class TaxonomyStore
{
public:
    explicit TaxonomyStore(QString connectionName);

    // Normalises a name for lookup: lower-cased, accents stripped, whitespace
    // collapsed. Public so callers can build matching queries the same way.
    // Does NOT touch the hybrid multiplication sign (×) -- stored taxon names
    // keep it as-is; see foldSearchText() for typed search input.
    static QString foldName(const QString &name);

    // Folds a user-typed search query the same way as foldName(), but first
    // swaps a standalone "x"/"X" token for the hybrid multiplication sign
    // (×) iNaturalist stores hybrid names with -- e.g. someone typing
    // "Eucalyptus x carolaniae" is looking for "Eucalyptus × carolaniae".
    // Never touches "x" inside a word, so ordinary names are unaffected.
    static QString foldSearchText(const QString &text);

    // True for a rank at or below species: species itself, the infraspecific
    // ranks, and the hybrid-formula ranks (genus and intraspecific).
    static bool isLeafRank(const QString &rank);

    // True for a rank that can appear as a direct child of a species taxon
    // (subspecies, variety, form, hybrid, infrahybrid).
    static bool isInfraspecificRank(const QString &rank);

    // --- writes -------------------------------------------------------------

    bool upsertPlace(const Place &place);

    // Inserts or updates the taxon (keyed by inat_id) and replaces its
    // taxon_name rows. Returns the local taxon.id, or -1 on failure.
    // Implemented in terms of upsertTaxa() below.
    int upsertTaxon(const Taxon &taxon);

    // Batched analogue of upsertTaxon(): one multi-row INSERT ... ON
    // CONFLICT for the taxa, one SELECT to resolve all local ids, one
    // DELETE + one multi-row INSERT for all their name rows combined -- a
    // constant handful of round trips regardless of list size, instead of
    // ~4 PER taxon. This is the difference between a reference-tree build
    // taking seconds and minutes against a remote Postgres catalogue.
    //
    // Deduplicates by inat_id (last one wins) before building SQL -- a
    // required correctness step, not an optimization: Postgres's
    // ON CONFLICT ... DO UPDATE raises "cannot affect row a second time" if
    // one multi-row INSERT targets the same conflicting key twice, whereas
    // SQLite's upsert tolerates it -- a real portability trap that would
    // pass local SQLite tests and fail against real Postgres. Also chunks
    // any list larger than a safe per-statement bind-parameter budget
    // (don't assume a build's SQLite raises the classic 999-parameter
    // default).
    //
    // Returns inat_id -> local id for every taxon written; empty on any
    // failure (nothing is partially applied by one call).
    QHash<qint64, int> upsertTaxa(const QList<Taxon> &taxa);

    bool addStatus(qint64 taxonInatId, const StatusRecord &status);

    // Adds one alternative name to an existing taxon (kept if already present).
    bool addName(qint64 taxonInatId, const QString &name, const QString &kind);

    // Returns the project id, creating the row if `name` is new.
    int ensureProject(const QString &name, std::optional<qint64> rootTaxonInatId,
                      std::optional<qint64> placeInatId, const QString &source);
    bool setProjectRefreshedNow(int projectId);

    // Implemented in terms of addProjectTaxa() below.
    bool addProjectTaxon(int projectId, qint64 taxonInatId, bool inRegion, bool fromChecklist);

    // One local taxon id + the project_taxon flags to write for it.
    struct ProjectTaxonLink
    {
        int taxonLocalId = 0;
        bool inRegion = false;
        bool fromChecklist = false;
    };

    // Batched analogue of addProjectTaxon(): one multi-row INSERT ... ON
    // CONFLICT for every link (same dedup-by-taxonLocalId + chunking rules
    // as upsertTaxa()), instead of one round trip per taxon. Takes
    // already-resolved local ids (e.g. from upsertTaxa()'s return) so a
    // caller that already has them doesn't pay a redundant lookup.
    bool addProjectTaxa(int projectId, const QList<ProjectTaxonLink> &links);

    // Resume point for an interrupted build/refresh's species-pagination
    // phase. saveBuildCheckpoint() is meant to be called inside the same
    // beginBatch()/commitBatch() transaction as the page's own writes, so the
    // checkpoint and the data it describes commit atomically. One row per
    // project -- saving replaces any previous checkpoint.
    bool saveBuildCheckpoint(int projectId, const BuildCheckpoint &checkpoint);
    std::optional<BuildCheckpoint> buildCheckpoint(int projectId) const;
    bool clearBuildCheckpoint(int projectId);

    // The iNat ids of every ancestor (by the shared taxon cache's `ancestry`
    // column) of the project's in-region species -- including ancestors that
    // were written by an earlier, interrupted run and so never entered this
    // run's own in-memory needed-ancestors set. ProjectBuilder::fillAncestors()
    // folds this in unconditionally so a resumed run's ancestor fill is
    // correct-by-construction rather than a resume-only special case.
    QList<qint64> projectSpeciesAncestorIds(int projectId) const;

    // Wraps a run of the write calls above in one transaction. Building a
    // reference tree can mean hundreds of upsertTaxon()/addProjectTaxon()
    // calls per network page; on a local SQLite file each is effectively
    // free, but on a shared Postgres connection every call is its own
    // network round trip, and each auto-committed statement separately pays
    // a commit-acknowledgement wait -- wrapping a page's worth of calls in
    // one transaction (one commit instead of many) cuts that overhead
    // sharply. Safe on both backends; a no-op improvement on SQLite.
    bool beginBatch();
    bool commitBatch();
    void rollbackBatch();

    // Deletes the project and its project_taxon / representative rows (cascaded by the
    // schema). The shared taxon cache and any matched captures are left untouched.
    bool deleteProject(int projectId);

    // How many of the project's current taxa would be removed by pruning
    // `taxonInatId` (itself plus every descendant by the shared taxon
    // cache's ancestry) -- for a confirmation prompt before committing.
    // 0 if the taxon isn't currently in the project's tree.
    int projectPruneCount(int projectId, qint64 taxonInatId) const;

    // Removes `taxonInatId` and everything beneath it from the project's
    // tree (the shared taxon cache and other projects are untouched), and
    // records the exclusion so a later refresh won't re-add any of them --
    // ProjectBuilder loads projectExcludedTaxonIds() once per refresh and
    // skips them, including taxa iNaturalist later adds under a pruned
    // group. Returns the number of project_taxon rows removed, or -1 on
    // error.
    int pruneTaxonFromProject(int projectId, qint64 taxonInatId);

    // True once anything has ever been pruned from this project.
    bool projectIsPruned(int projectId) const;

    // Every taxon id excluded from this project (self+descendants recorded
    // at prune time).
    QList<qint64> projectExcludedTaxonIds(int projectId) const;

    // Removes one exclusion row (a no-op success if none exists) -- the
    // inverse of pruneTaxonFromProject() for a single taxon. Called when a
    // previously-pruned taxon is explicitly re-added, so a later refresh can
    // discover new species under it / re-link it as a parent again, instead
    // of permanently treating it as still-pruned.
    bool clearProjectExclusion(int projectId, qint64 taxonInatId);

    // Upserts each ancestor (root-first) and `taxon` itself into the shared
    // cache, links them all into `projectId` (ancestors non-in-region;
    // `taxon` in-region iff isLeafRank(taxon.rank)), and clears any prior
    // prune-exclusion recorded against each of them -- see
    // clearProjectExclusion(). Mirrors the write sequence
    // ProjectBuilder::fetchRootDetail() uses for a project's own root spine.
    // Runs in its own transaction; rolls back and returns false if any step
    // fails.
    bool addTaxonWithAncestors(int projectId, const QList<Taxon> &ancestors, const Taxon &taxon);

    // The project's own root taxon id, or nullopt if it has none set.
    std::optional<qint64> projectRootTaxonInatId(int projectId) const;

    // The iNat ids of the project's species-rank taxa that haven't yet been checked
    // for infraspecific children (see InfraspecificFiller). When `scopeInatId` > 0,
    // only species at or below that taxon in the tree.
    QList<qint64> projectSpeciesNeedingInfraCheck(int projectId, qint64 scopeInatId = 0) const;

    // Marks a species as checked, so a later run doesn't query it again.
    bool markInfraChecked(int projectId, qint64 speciesInatId);

    // --- conditional-GET cache --------------------------------------------

    struct CacheEntry
    {
        QByteArray body;
        QString etag;
        QString lastModified;
        int status = 0;
    };
    std::optional<CacheEntry> cachedResponse(const QString &url) const;
    bool storeResponse(const QString &url, const QString &etag, const QString &lastModified,
                       int status, const QByteArray &body);

    // --- reads ------------------------------------------------------------

    std::optional<Place> placeByInatId(qint64 inatId) const;
    std::optional<qint64> taxonLocalId(qint64 inatId) const;

    // A cached taxon's name and iNaturalist reference photo (empty strings when
    // the column is null; `found` is false when the taxon isn't cached at all).
    struct TaxonPhoto
    {
        QString name;
        QString commonName;
        QString photoUrl;
        QString attribution;
        bool found = false;
    };
    TaxonPhoto taxonPhoto(qint64 inatId) const;

    // The iNat id of a taxon whose accepted or alternative name folds to `folded`
    // (accepted preferred). Lets a caller skip a network lookup for a name
    // already cached.
    std::optional<qint64> taxonInatIdByFoldedName(const QString &folded) const;

    // Distinct genus tokens (first word of the accepted name) among a project's
    // taxa, lower-cased.
    QStringList projectGenera(int projectId) const;

    int taxonCount() const;
    std::optional<int> projectIdByName(const QString &name) const;
    std::optional<qint64> projectPlaceInatId(int projectId) const;

    // The active locality's bounding box, or nullopt if the project has no
    // locality, that place isn't cached, or it has no bbox recorded.
    std::optional<geo::GeoBox> projectLocalityBox(int projectId) const;
    QList<qint64> projectTaxonInatIds(int projectId) const;

    // The project's tree, parents before children (breadth-ish via rank_level).
    QList<TreeNode> projectTree(int projectId) const;

    // One species (or infraspecific / hybrid leaf taxon) in a project, with its
    // cached iNaturalist reference photo if one has been fetched.
    struct LeafPhoto
    {
        qint64 inatId = 0;
        QString name;
        QString commonName;
        QString rank;
        QString photoUrl;        // empty until ReferencePhotoFetcher fills it
        QString attribution;
    };

    // The project's leaf-rank taxa (species and below), name-sorted. When
    // `scopeInatId` > 0, only those at or below that taxon in the tree.
    QList<LeafPhoto> projectLeafPhotos(int projectId, qint64 scopeInatId = 0) const;

    // Same, but restricted to exactly this set of taxon ids (no subtree walk)
    // -- for "every photo of taxa carrying conservation status X".
    QList<LeafPhoto> projectLeafPhotos(int projectId, const QList<qint64> &taxonInatIds) const;

    // iNat ids of the project's leaf-rank taxa that have no reference photo URL
    // cached yet (ReferencePhotoFetcher's work list).
    QList<qint64> projectLeafTaxaMissingPhoto(int projectId) const;

private:
    QString m_connectionName;
};

} // namespace pl::taxonomy
