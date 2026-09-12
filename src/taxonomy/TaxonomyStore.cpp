#include "taxonomy/TaxonomyStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::taxonomy {
namespace {

QVariant opt(const std::optional<qint64> &v)
{
    return v ? QVariant(qlonglong(*v)) : QVariant();
}
QVariant opt(const std::optional<int> &v)
{
    return v ? QVariant(*v) : QVariant();
}
QVariant opt(const std::optional<double> &v)
{
    return v ? QVariant(*v) : QVariant();
}

} // namespace

bool TaxonomyStore::isLeafRank(const QString &rank)
{
    static const QStringList leaf = {
        QStringLiteral("species"),    QStringLiteral("subspecies"), QStringLiteral("variety"),
        QStringLiteral("form"),       QStringLiteral("hybrid"),     QStringLiteral("infrahybrid"),
        QStringLiteral("genushybrid"),
    };
    return leaf.contains(rank.toLower());
}

// Ranks that can appear as a direct child of a species: the infraspecific
// ranks proper, plus intraspecific hybrid formulas. Excludes "genushybrid",
// which hangs off a genus, not a species.
bool TaxonomyStore::isInfraspecificRank(const QString &rank)
{
    static const QStringList infra = {
        QStringLiteral("subspecies"), QStringLiteral("variety"),
        QStringLiteral("form"),       QStringLiteral("hybrid"), QStringLiteral("infrahybrid"),
    };
    return infra.contains(rank.toLower());
}

TaxonomyStore::TaxonomyStore(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

QString TaxonomyStore::foldName(const QString &name)
{
    const QString decomposed = name.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        if (ch.category() == QChar::Mark_NonSpacing || ch.category() == QChar::Mark_SpacingCombining)
            continue;
        out.append(ch.toLower());
    }
    return out.simplified();
}

bool TaxonomyStore::upsertPlace(const Place &place)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO place (inat_id, name, display_name, admin_level, "
        "  bbox_swlat, bbox_swlng, bbox_nelat, bbox_nelng, fetched_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
        "ON CONFLICT(inat_id) DO UPDATE SET name = excluded.name, "
        "  display_name = excluded.display_name, admin_level = excluded.admin_level, "
        "  bbox_swlat = excluded.bbox_swlat, bbox_swlng = excluded.bbox_swlng, "
        "  bbox_nelat = excluded.bbox_nelat, bbox_nelng = excluded.bbox_nelng, "
        "  fetched_at = excluded.fetched_at"));
    q.addBindValue(qlonglong(place.inatId));
    q.addBindValue(place.name);
    q.addBindValue(place.displayName.isEmpty() ? QVariant() : place.displayName);
    q.addBindValue(opt(place.adminLevel));
    q.addBindValue(opt(place.bboxSwLat));
    q.addBindValue(opt(place.bboxSwLng));
    q.addBindValue(opt(place.bboxNeLat));
    q.addBindValue(opt(place.bboxNeLng));
    return q.exec();
}

int TaxonomyStore::upsertTaxon(const Taxon &taxon)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery up(db);
    up.prepare(QStringLiteral(
        "INSERT INTO taxon (inat_id, parent_inat_id, rank, rank_level, name, common_name, "
        "  ancestry, is_active, photo_url, photo_attribution, fetched_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
        "ON CONFLICT(inat_id) DO UPDATE SET parent_inat_id = excluded.parent_inat_id, "
        "  rank = excluded.rank, rank_level = excluded.rank_level, name = excluded.name, "
        "  common_name = excluded.common_name, ancestry = excluded.ancestry, "
        "  is_active = excluded.is_active, fetched_at = excluded.fetched_at, "
        // Keep an existing photo when this upsert carries none (most callers
        // don't request default_photo); a non-empty value always wins.
        "  photo_url = COALESCE(excluded.photo_url, taxon.photo_url), "
        "  photo_attribution = COALESCE(excluded.photo_attribution, taxon.photo_attribution)"));
    up.addBindValue(qlonglong(taxon.inatId));
    up.addBindValue(opt(taxon.parentInatId));
    up.addBindValue(taxon.rank);
    up.addBindValue(opt(taxon.rankLevel));
    up.addBindValue(taxon.name);
    up.addBindValue(taxon.commonName.isEmpty() ? QVariant() : taxon.commonName);
    up.addBindValue(taxon.ancestry.isEmpty() ? QVariant() : taxon.ancestry);
    up.addBindValue(taxon.isActive ? 1 : 0);
    up.addBindValue(taxon.photoUrl.isEmpty() ? QVariant() : taxon.photoUrl);
    up.addBindValue(taxon.photoAttribution.isEmpty() ? QVariant() : taxon.photoAttribution);
    if (!up.exec())
        return -1;

    QSqlQuery idq(db);
    idq.prepare(QStringLiteral("SELECT id FROM taxon WHERE inat_id = ?"));
    idq.addBindValue(qlonglong(taxon.inatId));
    if (!idq.exec() || !idq.next())
        return -1;
    const int localId = idq.value(0).toInt();

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM taxon_name WHERE taxon_id = ?"));
    del.addBindValue(localId);
    del.exec();

    auto addName = [&](const QString &name, const QString &kind) {
        if (name.trimmed().isEmpty())
            return;
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO taxon_name (taxon_id, name, name_folded, kind) "
            "VALUES (?, ?, ?, ?)"));
        ins.addBindValue(localId);
        ins.addBindValue(name.trimmed());
        ins.addBindValue(foldName(name));
        ins.addBindValue(kind);
        ins.exec();
    };

    addName(taxon.name, QStringLiteral("accepted"));
    for (const QString &syn : taxon.synonyms)
        addName(syn, QStringLiteral("synonym"));
    for (const QString &vern : taxon.vernacular)
        addName(vern, QStringLiteral("vernacular"));

    return localId;
}

bool TaxonomyStore::addStatus(qint64 taxonInatId, const StatusRecord &status)
{
    const auto local = taxonLocalId(taxonInatId);
    if (!local)
        return false;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO taxon_status (taxon_id, place_inat_id, status, status_folded, source) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(taxon_id, place_inat_id, source) DO UPDATE SET "
        "  status = excluded.status, status_folded = excluded.status_folded"));
    q.addBindValue(qlonglong(*local));
    q.addBindValue(opt(status.placeInatId));
    q.addBindValue(status.status);
    q.addBindValue(foldName(status.status));
    q.addBindValue(status.source.isEmpty() ? QVariant() : status.source);
    return q.exec();
}

bool TaxonomyStore::addName(qint64 taxonInatId, const QString &name, const QString &kind)
{
    const auto local = taxonLocalId(taxonInatId);
    if (!local || name.trimmed().isEmpty())
        return false;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO taxon_name (taxon_id, name, name_folded, kind) VALUES (?, ?, ?, ?)"));
    q.addBindValue(qlonglong(*local));
    q.addBindValue(name.trimmed());
    q.addBindValue(foldName(name));
    q.addBindValue(kind);
    return q.exec();
}

std::optional<qint64> TaxonomyStore::taxonInatIdByFoldedName(const QString &folded) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
        "WHERE tn.name_folded = ? ORDER BY (tn.kind = 'accepted') DESC LIMIT 1"));
    q.addBindValue(folded);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

QStringList TaxonomyStore::projectGenera(int projectId) const
{
    QStringList genera;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT DISTINCT lower(substr(t.name, 1, "
        "  CASE WHEN instr(t.name, ' ') > 0 THEN instr(t.name, ' ') - 1 ELSE length(t.name) END)) "
        "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id WHERE pt.project_id = ?"));
    q.addBindValue(projectId);
    if (q.exec()) {
        while (q.next())
            genera << q.value(0).toString();
    }
    return genera;
}

int TaxonomyStore::ensureProject(const QString &name, std::optional<qint64> rootTaxonInatId,
                                 std::optional<qint64> placeInatId, const QString &source)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO project (name, root_taxon_inat_id, place_inat_id, source) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET root_taxon_inat_id = excluded.root_taxon_inat_id, "
        "  place_inat_id = excluded.place_inat_id, source = excluded.source"));
    ins.addBindValue(name);
    ins.addBindValue(opt(rootTaxonInatId));
    ins.addBindValue(opt(placeInatId));
    ins.addBindValue(source.isEmpty() ? QStringLiteral("inat") : source);
    if (!ins.exec())
        return -1;

    const auto id = projectIdByName(name);
    return id ? *id : -1;
}

bool TaxonomyStore::setProjectRefreshedNow(int projectId)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "UPDATE project SET refreshed_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?"));
    q.addBindValue(projectId);
    return q.exec();
}

bool TaxonomyStore::deleteProject(int projectId)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("DELETE FROM project WHERE id = ?"));
    q.addBindValue(projectId);
    return q.exec();
}

QList<qint64> TaxonomyStore::projectSpeciesNeedingInfraCheck(int projectId,
                                                             qint64 scopeInatId) const
{
    QList<qint64> ids;
    // A blanket sweep skips species already checked (so it is resumable); an
    // explicitly scoped request re-checks everything under the chosen taxon.
    QString sql = QStringLiteral(
        "SELECT t.inat_id FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ? AND t.rank = 'species'");
    if (scopeInatId <= 0)
        sql += QStringLiteral(" AND pt.infra_checked = 0");
    if (scopeInatId > 0) {
        sql += QStringLiteral(
            " AND t.inat_id IN ("
            "  WITH RECURSIVE sub(x) AS (SELECT ? "
            "    UNION ALL SELECT c.inat_id FROM taxon c JOIN sub ON c.parent_inat_id = sub.x) "
            "  SELECT x FROM sub)");
    }

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(sql);
    q.addBindValue(projectId);
    if (scopeInatId > 0)
        q.addBindValue(qlonglong(scopeInatId));
    if (!q.exec())
        return ids;
    while (q.next())
        ids.append(q.value(0).toLongLong());
    return ids;
}

bool TaxonomyStore::markInfraChecked(int projectId, qint64 speciesInatId)
{
    const auto local = taxonLocalId(speciesInatId);
    if (!local)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "UPDATE project_taxon SET infra_checked = 1 WHERE project_id = ? AND taxon_id = ?"));
    q.addBindValue(projectId);
    q.addBindValue(qlonglong(*local));
    return q.exec();
}

bool TaxonomyStore::addProjectTaxon(int projectId, qint64 taxonInatId, bool inRegion,
                                    bool fromChecklist)
{
    const auto local = taxonLocalId(taxonInatId);
    if (!local)
        return false;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO project_taxon (project_id, taxon_id, in_region, from_checklist) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(project_id, taxon_id) DO UPDATE SET "
        "  in_region = max(project_taxon.in_region, excluded.in_region), "
        "  from_checklist = max(project_taxon.from_checklist, excluded.from_checklist)"));
    q.addBindValue(projectId);
    q.addBindValue(qlonglong(*local));
    q.addBindValue(inRegion ? 1 : 0);
    q.addBindValue(fromChecklist ? 1 : 0);
    return q.exec();
}

std::optional<TaxonomyStore::CacheEntry> TaxonomyStore::cachedResponse(const QString &url) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT body, etag, last_modified, status FROM http_cache WHERE url = ?"));
    q.addBindValue(url);
    if (!q.exec() || !q.next())
        return std::nullopt;

    CacheEntry e;
    e.body = q.value(0).toByteArray();
    e.etag = q.value(1).toString();
    e.lastModified = q.value(2).toString();
    e.status = q.value(3).toInt();
    return e;
}

bool TaxonomyStore::storeResponse(const QString &url, const QString &etag,
                                  const QString &lastModified, int status, const QByteArray &body)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO http_cache (url, etag, last_modified, status, body, fetched_at) "
        "VALUES (?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
        "ON CONFLICT(url) DO UPDATE SET etag = excluded.etag, "
        "  last_modified = excluded.last_modified, status = excluded.status, "
        "  body = excluded.body, fetched_at = excluded.fetched_at"));
    q.addBindValue(url);
    q.addBindValue(etag.isEmpty() ? QVariant() : etag);
    q.addBindValue(lastModified.isEmpty() ? QVariant() : lastModified);
    q.addBindValue(status);
    q.addBindValue(body);
    return q.exec();
}

std::optional<Place> TaxonomyStore::placeByInatId(qint64 inatId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT name, display_name, admin_level, bbox_swlat, bbox_swlng, bbox_nelat, bbox_nelng "
        "FROM place WHERE inat_id = ?"));
    q.addBindValue(qlonglong(inatId));
    if (!q.exec() || !q.next())
        return std::nullopt;

    Place p;
    p.inatId = inatId;
    p.name = q.value(0).toString();
    p.displayName = q.value(1).toString();
    if (!q.value(2).isNull())
        p.adminLevel = q.value(2).toInt();
    if (!q.value(3).isNull())
        p.bboxSwLat = q.value(3).toDouble();
    if (!q.value(4).isNull())
        p.bboxSwLng = q.value(4).toDouble();
    if (!q.value(5).isNull())
        p.bboxNeLat = q.value(5).toDouble();
    if (!q.value(6).isNull())
        p.bboxNeLng = q.value(6).toDouble();
    return p;
}

std::optional<qint64> TaxonomyStore::taxonLocalId(qint64 inatId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT id FROM taxon WHERE inat_id = ?"));
    q.addBindValue(qlonglong(inatId));
    if (!q.exec() || !q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

TaxonomyStore::TaxonPhoto TaxonomyStore::taxonPhoto(qint64 inatId) const
{
    TaxonPhoto out;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT name, common_name, photo_url, photo_attribution FROM taxon WHERE inat_id = ?"));
    q.addBindValue(qlonglong(inatId));
    if (!q.exec() || !q.next())
        return out;
    out.found = true;
    out.name = q.value(0).toString();
    out.commonName = q.value(1).toString();
    out.photoUrl = q.value(2).toString();
    out.attribution = q.value(3).toString();
    return out;
}

int TaxonomyStore::taxonCount() const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM taxon")) || !q.next())
        return -1;
    return q.value(0).toInt();
}

std::optional<int> TaxonomyStore::projectIdByName(const QString &name) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT id FROM project WHERE name = ?"));
    q.addBindValue(name);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return q.value(0).toInt();
}

std::optional<qint64> TaxonomyStore::projectPlaceInatId(int projectId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT place_inat_id FROM project WHERE id = ?"));
    q.addBindValue(projectId);
    if (!q.exec() || !q.next() || q.value(0).isNull())
        return std::nullopt;
    return q.value(0).toLongLong();
}

std::optional<geo::GeoBox> TaxonomyStore::projectLocalityBox(int projectId) const
{
    const auto placeId = projectPlaceInatId(projectId);
    if (!placeId)
        return std::nullopt;
    const auto place = placeByInatId(*placeId);
    if (!place || !place->bboxSwLat || !place->bboxSwLng || !place->bboxNeLat || !place->bboxNeLng)
        return std::nullopt;
    return geo::GeoBox{*place->bboxSwLat, *place->bboxSwLng, *place->bboxNeLat, *place->bboxNeLng};
}

QList<qint64> TaxonomyStore::projectTaxonInatIds(int projectId) const
{
    QList<qint64> ids;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ?"));
    q.addBindValue(projectId);
    if (q.exec()) {
        while (q.next())
            ids.append(q.value(0).toLongLong());
    }
    return ids;
}

QList<TreeNode> TaxonomyStore::projectTree(int projectId) const
{
    QList<TreeNode> nodes;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id, t.parent_inat_id, t.rank, t.name, t.common_name, pt.in_region "
        "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ? "
        "ORDER BY COALESCE(t.rank_level, 0) DESC, t.name"));
    q.addBindValue(projectId);
    if (!q.exec())
        return nodes;

    while (q.next()) {
        TreeNode n;
        n.inatId = q.value(0).toLongLong();
        if (!q.value(1).isNull())
            n.parentInatId = q.value(1).toLongLong();
        n.rank = q.value(2).toString();
        n.name = q.value(3).toString();
        n.commonName = q.value(4).toString();
        n.inRegion = q.value(5).toInt() != 0;
        n.isLeafRank = TaxonomyStore::isLeafRank(n.rank);
        nodes.append(n);
    }
    return nodes;
}

QList<TaxonomyStore::LeafPhoto> TaxonomyStore::projectLeafPhotos(int projectId,
                                                                qint64 scopeInatId) const
{
    QList<LeafPhoto> out;
    QString sql = QStringLiteral(
        "SELECT t.inat_id, t.name, t.common_name, t.rank, t.photo_url, t.photo_attribution "
        "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ?");
    if (scopeInatId > 0) {
        sql += QStringLiteral(
            " AND t.inat_id IN ("
            "  WITH RECURSIVE sub(x) AS (SELECT ? "
            "    UNION ALL SELECT c.inat_id FROM taxon c JOIN sub ON c.parent_inat_id = sub.x) "
            "  SELECT x FROM sub)");
    }
    sql += QStringLiteral(" ORDER BY t.name");

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(sql);
    q.addBindValue(projectId);
    if (scopeInatId > 0)
        q.addBindValue(qlonglong(scopeInatId));
    if (!q.exec())
        return out;

    while (q.next()) {
        const QString rank = q.value(3).toString();
        if (!isLeafRank(rank))
            continue;
        LeafPhoto p;
        p.inatId = q.value(0).toLongLong();
        p.name = q.value(1).toString();
        p.commonName = q.value(2).toString();
        p.rank = rank;
        p.photoUrl = q.value(4).toString();
        p.attribution = q.value(5).toString();
        out.append(p);
    }
    return out;
}

QList<qint64> TaxonomyStore::projectLeafTaxaMissingPhoto(int projectId) const
{
    QList<qint64> ids;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id, t.rank FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ? AND (t.photo_url IS NULL OR t.photo_url = '') "
        "ORDER BY t.inat_id"));
    q.addBindValue(projectId);
    if (!q.exec())
        return ids;
    while (q.next()) {
        if (isLeafRank(q.value(1).toString()))
            ids.append(q.value(0).toLongLong());
    }
    return ids;
}

} // namespace pl::taxonomy
