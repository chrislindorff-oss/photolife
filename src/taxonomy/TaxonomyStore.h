#pragma once

#include "geo/GeoBox.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QByteArray>
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
    static QString foldName(const QString &name);

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
    int upsertTaxon(const Taxon &taxon);

    bool addStatus(qint64 taxonInatId, const StatusRecord &status);

    // Adds one alternative name to an existing taxon (kept if already present).
    bool addName(qint64 taxonInatId, const QString &name, const QString &kind);

    // Returns the project id, creating the row if `name` is new.
    int ensureProject(const QString &name, std::optional<qint64> rootTaxonInatId,
                      std::optional<qint64> placeInatId, const QString &source);
    bool setProjectRefreshedNow(int projectId);

    bool addProjectTaxon(int projectId, qint64 taxonInatId, bool inRegion, bool fromChecklist);

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

    // iNat ids of the project's leaf-rank taxa that have no reference photo URL
    // cached yet (ReferencePhotoFetcher's work list).
    QList<qint64> projectLeafTaxaMissingPhoto(int projectId) const;

private:
    QString m_connectionName;
};

} // namespace pl::taxonomy
