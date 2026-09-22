#include "taxonomy/TaxonomyStore.h"

#include "db/Database.h"
#include "net/INatParse.h"

#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

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

// Safe bind-parameter budget for one multi-row INSERT built by
// upsertTaxa()/addProjectTaxa() -- comfortably under SQLite's classic
// default SQLITE_LIMIT_VARIABLE_NUMBER of 999 (don't assume a given build
// links a newer SQLite that raises it). Rows-per-statement is this divided
// by the number of bind params per row, so a wide table (taxon: 10) chunks
// more aggressively than a narrow one (taxon_name/project_taxon: 4).
constexpr int kMaxBatchBindParams = 900;

// Local taxon.id of `taxonInatId` and every descendant beneath it, by the
// shared taxon cache's parent_inat_id chain (the "prune this group" fan-out).
// Includes the taxon itself. Empty on a query failure.
QList<qint64> subtreeLocalIds(QSqlDatabase &db, qint64 taxonInatId)
{
    QList<qint64> ids;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "WITH RECURSIVE sub(inat_id) AS ("
        "  SELECT ? "
        "  UNION ALL "
        "  SELECT t.inat_id FROM taxon t JOIN sub ON t.parent_inat_id = sub.inat_id"
        ") "
        "SELECT t.id FROM taxon t JOIN sub ON t.inat_id = sub.inat_id"));
    q.addBindValue(taxonInatId);
    if (!q.exec())
        return ids;
    while (q.next())
        ids.append(q.value(0).toLongLong());
    return ids;
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

QString TaxonomyStore::foldSearchText(const QString &text)
{
    static const QRegularExpression hybridToken(QStringLiteral("(?<=^|\\s)[xX](?=\\s|$)"));
    QString normalized = text;
    normalized.replace(hybridToken, QString::fromUtf8("\xC3\x97"));   // U+00D7
    return foldName(normalized);
}

bool TaxonomyStore::upsertPlace(const Place &place)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO place (inat_id, name, display_name, admin_level, "
        "  bbox_swlat, bbox_swlng, bbox_nelat, bbox_nelng, fetched_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, %1) "
        "ON CONFLICT(inat_id) DO UPDATE SET name = excluded.name, "
        "  display_name = excluded.display_name, admin_level = excluded.admin_level, "
        "  bbox_swlat = excluded.bbox_swlat, bbox_swlng = excluded.bbox_swlng, "
        "  bbox_nelat = excluded.bbox_nelat, bbox_nelng = excluded.bbox_nelng, "
        "  fetched_at = excluded.fetched_at")
                    .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
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
    const QHash<qint64, int> result = upsertTaxa({taxon});
    const auto it = result.constFind(taxon.inatId);
    return it == result.constEnd() ? -1 : it.value();
}

QHash<qint64, int> TaxonomyStore::upsertTaxa(const QList<Taxon> &taxa)
{
    QHash<qint64, int> result;
    if (taxa.isEmpty())
        return result;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    // Dedupe by inat_id (last one wins) -- see the header comment: a
    // multi-row Postgres UPSERT can't target the same conflicting key twice
    // in one statement.
    QList<Taxon> deduped;
    QHash<qint64, int> indexByInatId;
    for (const Taxon &t : taxa) {
        const auto it = indexByInatId.constFind(t.inatId);
        if (it == indexByInatId.constEnd()) {
            indexByInatId.insert(t.inatId, int(deduped.size()));
            deduped.append(t);
        } else {
            deduped[it.value()] = t;
        }
    }

    const QString nowExpr = Database::nowIsoExpr(Database::backendFor(m_connectionName));
    constexpr int kParamsPerRow = 10;
    const int chunkSize = std::max(1, kMaxBatchBindParams / kParamsPerRow);

    for (int offset = 0; offset < deduped.size(); offset += chunkSize) {
        const QList<Taxon> chunk = deduped.mid(offset, chunkSize);
        const QStringList rowPlaceholders(chunk.size(),
                                          QStringLiteral("(?,?,?,?,?,?,?,?,?,?,%1)"));

        QSqlQuery up(db);
        up.prepare((QStringLiteral(
            "INSERT INTO taxon (inat_id, parent_inat_id, rank, rank_level, name, common_name, "
            "  ancestry, is_active, photo_url, photo_attribution, fetched_at) VALUES ")
                    + rowPlaceholders.join(QLatin1Char(','))
                    + QStringLiteral(
            " ON CONFLICT(inat_id) DO UPDATE SET parent_inat_id = excluded.parent_inat_id, "
            "  rank = excluded.rank, rank_level = excluded.rank_level, name = excluded.name, "
            "  common_name = excluded.common_name, ancestry = excluded.ancestry, "
            "  is_active = excluded.is_active, fetched_at = excluded.fetched_at, "
            // Keep an existing photo when this upsert carries none (most
            // callers don't request default_photo); a non-empty value wins.
            "  photo_url = COALESCE(excluded.photo_url, taxon.photo_url), "
            "  photo_attribution = COALESCE(excluded.photo_attribution, taxon.photo_attribution)"))
                       .arg(nowExpr));
        for (const Taxon &t : chunk) {
            up.addBindValue(qlonglong(t.inatId));
            up.addBindValue(opt(t.parentInatId));
            up.addBindValue(t.rank);
            up.addBindValue(opt(t.rankLevel));
            up.addBindValue(t.name);
            up.addBindValue(t.commonName.isEmpty() ? QVariant() : t.commonName);
            up.addBindValue(t.ancestry.isEmpty() ? QVariant() : t.ancestry);
            up.addBindValue(t.isActive ? 1 : 0);
            up.addBindValue(t.photoUrl.isEmpty() ? QVariant() : t.photoUrl);
            up.addBindValue(t.photoAttribution.isEmpty() ? QVariant() : t.photoAttribution);
        }
        if (!up.exec())
            return {};
    }

    // Resolve local ids for every upserted taxon in one round trip.
    {
        const QStringList placeholders(deduped.size(), QStringLiteral("?"));
        QSqlQuery idq(db);
        idq.prepare(QStringLiteral("SELECT id, inat_id FROM taxon WHERE inat_id IN (%1)")
                        .arg(placeholders.join(QLatin1Char(','))));
        for (const Taxon &t : deduped)
            idq.addBindValue(qlonglong(t.inatId));
        if (!idq.exec())
            return {};
        while (idq.next())
            result.insert(idq.value(1).toLongLong(), idq.value(0).toInt());
    }
    if (result.size() != deduped.size())
        return {};

    // Replace every one of these taxa's name rows: one delete, then one
    // multi-row insert for all of them combined.
    const QList<int> localIds = result.values();
    {
        const QStringList placeholders(localIds.size(), QStringLiteral("?"));
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM taxon_name WHERE taxon_id IN (%1)")
                        .arg(placeholders.join(QLatin1Char(','))));
        for (int id : localIds)
            del.addBindValue(id);
        if (!del.exec())
            return {};
    }

    struct NameRow
    {
        int taxonLocalId;
        QString name;
        QString kind;
    };
    QList<NameRow> names;
    QSet<QString> seenNameKeys;   // dedupe (taxon_id, name, kind) -- see above
    for (const Taxon &t : deduped) {
        const int localId = result.value(t.inatId);
        auto addName = [&](const QString &name, const QString &kind) {
            const QString trimmed = name.trimmed();
            if (trimmed.isEmpty())
                return;
            const QString key = QString::number(localId) + QLatin1Char('\x1f') + kind
                + QLatin1Char('\x1f') + trimmed;
            if (seenNameKeys.contains(key))
                return;
            seenNameKeys.insert(key);
            names.append({localId, trimmed, kind});
        };
        addName(t.name, QStringLiteral("accepted"));
        for (const QString &syn : t.synonyms)
            addName(syn, QStringLiteral("synonym"));
        for (const QString &vern : t.vernacular)
            addName(vern, QStringLiteral("vernacular"));
    }

    constexpr int kNameParamsPerRow = 4;
    const int nameChunkSize = std::max(1, kMaxBatchBindParams / kNameParamsPerRow);
    for (int offset = 0; offset < names.size(); offset += nameChunkSize) {
        const QList<NameRow> chunk = names.mid(offset, nameChunkSize);
        const QStringList rowPlaceholders(chunk.size(), QStringLiteral("(?,?,?,?)"));

        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO taxon_name (taxon_id, name, name_folded, kind) VALUES ")
                    + rowPlaceholders.join(QLatin1Char(','))
                    + QStringLiteral(" ON CONFLICT(taxon_id, name, kind) DO NOTHING"));
        for (const NameRow &n : chunk) {
            ins.addBindValue(n.taxonLocalId);
            ins.addBindValue(n.name);
            ins.addBindValue(foldName(n.name));
            ins.addBindValue(n.kind);
        }
        if (!ins.exec())
            return {};
    }

    return result;
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
        "INSERT INTO taxon_name (taxon_id, name, name_folded, kind) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(taxon_id, name, kind) DO NOTHING"));
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
    // The genus is the text before the first space (or the whole name, for
    // an unspaced higher-rank name). instr()/substr() is SQLite-only;
    // split_part() is the Postgres equivalent with the same "no space ->
    // whole string" behaviour.
    const QString genusExpr = Database::backendFor(m_connectionName) == CatalogueDescriptor::Backend::Postgres
        ? QStringLiteral("lower(split_part(t.name, ' ', 1))")
        : QStringLiteral(
              "lower(substr(t.name, 1, "
              "  CASE WHEN instr(t.name, ' ') > 0 THEN instr(t.name, ' ') - 1 ELSE length(t.name) END))");
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT DISTINCT %1 "
        "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id WHERE pt.project_id = ?")
                  .arg(genusExpr));
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
    q.prepare(QStringLiteral("UPDATE project SET refreshed_at = %1 WHERE id = ?")
                  .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
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

int TaxonomyStore::projectPruneCount(int projectId, qint64 taxonInatId) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    const QList<qint64> ids = subtreeLocalIds(db, taxonInatId);
    if (ids.isEmpty())
        return 0;

    QStringList marks;
    marks.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i)
        marks << QStringLiteral("?");

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
                  "SELECT COUNT(*) FROM project_taxon WHERE project_id = ? AND taxon_id IN (%1)")
                  .arg(marks.join(QLatin1Char(','))));
    q.addBindValue(projectId);
    for (qint64 id : ids)
        q.addBindValue(id);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toInt();
}

int TaxonomyStore::pruneTaxonFromProject(int projectId, qint64 taxonInatId)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    const QList<qint64> ids = subtreeLocalIds(db, taxonInatId);
    if (ids.isEmpty())
        return 0;

    if (!db.transaction())
        return -1;

    QStringList marks;
    marks.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i)
        marks << QStringLiteral("?");

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
                    "DELETE FROM project_taxon WHERE project_id = ? AND taxon_id IN (%1)")
                    .arg(marks.join(QLatin1Char(','))));
    del.addBindValue(projectId);
    for (qint64 id : ids)
        del.addBindValue(id);
    if (!del.exec()) {
        db.rollback();
        return -1;
    }
    const int removed = del.numRowsAffected();

    for (qint64 id : ids) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO project_excluded_taxon (project_id, taxon_id) VALUES (?, ?) "
            "ON CONFLICT (project_id, taxon_id) DO NOTHING"));
        ins.addBindValue(projectId);
        ins.addBindValue(id);
        if (!ins.exec()) {
            db.rollback();
            return -1;
        }
    }

    if (!db.commit())
        return -1;
    return removed;
}

bool TaxonomyStore::projectIsPruned(int projectId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT 1 FROM project_excluded_taxon WHERE project_id = ? LIMIT 1"));
    q.addBindValue(projectId);
    return q.exec() && q.next();
}

QList<qint64> TaxonomyStore::projectExcludedTaxonIds(int projectId) const
{
    QList<qint64> ids;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id FROM project_excluded_taxon pe "
        "JOIN taxon t ON t.id = pe.taxon_id WHERE pe.project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec())
        return ids;
    while (q.next())
        ids.append(q.value(0).toLongLong());
    return ids;
}

bool TaxonomyStore::clearProjectExclusion(int projectId, qint64 taxonInatId)
{
    const auto local = taxonLocalId(taxonInatId);
    if (!local)
        return true;   // not cached, so it can't be excluded either

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "DELETE FROM project_excluded_taxon WHERE project_id = ? AND taxon_id = ?"));
    q.addBindValue(projectId);
    q.addBindValue(qlonglong(*local));
    return q.exec();
}

bool TaxonomyStore::addTaxonWithAncestors(int projectId, const QList<Taxon> &ancestors,
                                          const Taxon &taxon)
{
    if (!beginBatch())
        return false;

    auto link = [&](const Taxon &t, bool inRegion) {
        if (upsertTaxon(t) < 0)
            return false;
        if (!addProjectTaxon(projectId, t.inatId, inRegion, false))
            return false;
        return clearProjectExclusion(projectId, t.inatId);
    };

    bool ok = true;
    for (const Taxon &ancestor : ancestors) {
        if (!link(ancestor, false)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = link(taxon, isLeafRank(taxon.rank));

    if (!ok) {
        rollbackBatch();
        return false;
    }
    return commitBatch();
}

std::optional<qint64> TaxonomyStore::projectRootTaxonInatId(int projectId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT root_taxon_inat_id FROM project WHERE id = ?"));
    q.addBindValue(projectId);
    if (!q.exec() || !q.next() || q.value(0).isNull())
        return std::nullopt;
    return q.value(0).toLongLong();
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

bool TaxonomyStore::beginBatch()
{
    return QSqlDatabase::database(m_connectionName, false).transaction();
}

bool TaxonomyStore::commitBatch()
{
    return QSqlDatabase::database(m_connectionName, false).commit();
}

void TaxonomyStore::rollbackBatch()
{
    QSqlDatabase::database(m_connectionName, false).rollback();
}

bool TaxonomyStore::addProjectTaxon(int projectId, qint64 taxonInatId, bool inRegion,
                                    bool fromChecklist)
{
    const auto local = taxonLocalId(taxonInatId);
    if (!local)
        return false;
    return addProjectTaxa(projectId, {{int(*local), inRegion, fromChecklist}});
}

bool TaxonomyStore::addProjectTaxa(int projectId, const QList<ProjectTaxonLink> &links)
{
    if (links.isEmpty())
        return true;

    // Dedupe by taxonLocalId (merging flags with OR, matching the DB's own
    // ON CONFLICT merge) -- same "can't target a conflicting key twice in
    // one multi-row UPSERT" reasoning as upsertTaxa().
    QHash<int, int> indexByLocalId;
    QList<ProjectTaxonLink> deduped;
    for (const ProjectTaxonLink &link : links) {
        const auto it = indexByLocalId.constFind(link.taxonLocalId);
        if (it == indexByLocalId.constEnd()) {
            indexByLocalId.insert(link.taxonLocalId, int(deduped.size()));
            deduped.append(link);
        } else {
            ProjectTaxonLink &existing = deduped[it.value()];
            existing.inRegion = existing.inRegion || link.inRegion;
            existing.fromChecklist = existing.fromChecklist || link.fromChecklist;
        }
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    constexpr int kParamsPerRow = 4;
    const int chunkSize = std::max(1, kMaxBatchBindParams / kParamsPerRow);

    for (int offset = 0; offset < deduped.size(); offset += chunkSize) {
        const QList<ProjectTaxonLink> chunk = deduped.mid(offset, chunkSize);
        const QStringList rowPlaceholders(chunk.size(), QStringLiteral("(?,?,?,?)"));

        QSqlQuery q(db);
        // "|" (bitwise OR) instead of max(a, b): SQLite's max() is a scalar
        // function when given 2+ args (returns the larger value), but Postgres's
        // max() is aggregate-only and has no such overload -- a | b gives the
        // same result as max(a, b) here specifically because these columns are
        // always exactly 0 or 1, never any other integer.
        q.prepare(QStringLiteral(
            "INSERT INTO project_taxon (project_id, taxon_id, in_region, from_checklist) VALUES ")
                  + rowPlaceholders.join(QLatin1Char(','))
                  + QStringLiteral(
            " ON CONFLICT(project_id, taxon_id) DO UPDATE SET "
            "  in_region = project_taxon.in_region | excluded.in_region, "
            "  from_checklist = project_taxon.from_checklist | excluded.from_checklist"));
        for (const ProjectTaxonLink &link : chunk) {
            q.addBindValue(projectId);
            q.addBindValue(link.taxonLocalId);
            q.addBindValue(link.inRegion ? 1 : 0);
            q.addBindValue(link.fromChecklist ? 1 : 0);
        }
        if (!q.exec())
            return false;
    }
    return true;
}

bool TaxonomyStore::saveBuildCheckpoint(int projectId, const BuildCheckpoint &checkpoint)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO project_build_checkpoint "
        "  (project_id, root_taxon_inat_id, place_inat_id, per_page, next_page, "
        "   species_seen, species_total, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, %1) "
        "ON CONFLICT(project_id) DO UPDATE SET "
        "  root_taxon_inat_id = excluded.root_taxon_inat_id, "
        "  place_inat_id = excluded.place_inat_id, per_page = excluded.per_page, "
        "  next_page = excluded.next_page, species_seen = excluded.species_seen, "
        "  species_total = excluded.species_total, updated_at = excluded.updated_at")
                    .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
    q.addBindValue(projectId);
    q.addBindValue(qlonglong(checkpoint.rootTaxonInatId));
    q.addBindValue(opt(checkpoint.placeInatId));
    q.addBindValue(checkpoint.perPage);
    q.addBindValue(checkpoint.nextPage);
    q.addBindValue(checkpoint.speciesSeen);
    q.addBindValue(checkpoint.speciesTotal);
    return q.exec();
}

std::optional<BuildCheckpoint> TaxonomyStore::buildCheckpoint(int projectId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT root_taxon_inat_id, place_inat_id, per_page, next_page, species_seen, "
        "       species_total "
        "FROM project_build_checkpoint WHERE project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec() || !q.next())
        return std::nullopt;

    BuildCheckpoint c;
    c.rootTaxonInatId = q.value(0).toLongLong();
    if (!q.value(1).isNull())
        c.placeInatId = q.value(1).toLongLong();
    c.perPage = q.value(2).toInt();
    c.nextPage = q.value(3).toInt();
    c.speciesSeen = q.value(4).toInt();
    c.speciesTotal = q.value(5).toInt();
    return c;
}

bool TaxonomyStore::clearBuildCheckpoint(int projectId)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("DELETE FROM project_build_checkpoint WHERE project_id = ?"));
    q.addBindValue(projectId);
    return q.exec();
}

QList<qint64> TaxonomyStore::projectSpeciesAncestorIds(int projectId) const
{
    QSet<qint64> ids;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT t.ancestry FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ? AND pt.in_region = 1"));
    q.addBindValue(projectId);
    if (!q.exec())
        return {};
    while (q.next()) {
        for (qint64 id : pl::net::inat::ancestryIds(q.value(0).toString()))
            ids.insert(id);
    }
    return QList<qint64>(ids.begin(), ids.end());
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
        "VALUES (?, ?, ?, ?, ?, %1) "
        "ON CONFLICT(url) DO UPDATE SET etag = excluded.etag, "
        "  last_modified = excluded.last_modified, status = excluded.status, "
        "  body = excluded.body, fetched_at = excluded.fetched_at")
                    .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
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

QList<TaxonomyStore::LeafPhoto> TaxonomyStore::projectLeafPhotos(
    int projectId, const QList<qint64> &taxonInatIds) const
{
    QList<LeafPhoto> out;
    if (taxonInatIds.isEmpty())
        return out;

    QStringList placeholders(taxonInatIds.size(), QStringLiteral("?"));
    const QString sql = QStringLiteral(
        "SELECT t.inat_id, t.name, t.common_name, t.rank, t.photo_url, t.photo_attribution "
        "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id "
        "WHERE pt.project_id = ? AND t.inat_id IN (%1) "
        "ORDER BY t.name").arg(placeholders.join(QLatin1Char(',')));

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(sql);
    q.addBindValue(projectId);
    for (qint64 id : taxonInatIds)
        q.addBindValue(qlonglong(id));
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
