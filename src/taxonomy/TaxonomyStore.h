#pragma once

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

    // --- writes -------------------------------------------------------------

    bool upsertPlace(const Place &place);

    // Inserts or updates the taxon (keyed by inat_id) and replaces its
    // taxon_name rows. Returns the local taxon.id, or -1 on failure.
    int upsertTaxon(const Taxon &taxon);

    bool addStatus(qint64 taxonInatId, const StatusRecord &status);

    // Returns the project id, creating the row if `name` is new.
    int ensureProject(const QString &name, std::optional<qint64> rootTaxonInatId,
                      std::optional<qint64> placeInatId, const QString &source);
    bool setProjectRefreshedNow(int projectId);

    bool addProjectTaxon(int projectId, qint64 taxonInatId, bool inRegion, bool fromChecklist);

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
    int taxonCount() const;
    std::optional<int> projectIdByName(const QString &name) const;
    QList<qint64> projectTaxonInatIds(int projectId) const;

    // The project's tree, parents before children (breadth-ish via rank_level).
    QList<TreeNode> projectTree(int projectId) const;

private:
    QString m_connectionName;
};

} // namespace pl::taxonomy
