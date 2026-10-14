#include "libsync/PhotoSelection.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace pl::libsync {

namespace {

// Captures with an auto or confirmed match into a tree (any tree, or one of
// `trees` when given).
const char *kInTrees =
    "SELECT DISTINCT m.capture_id FROM capture_match m "
    "JOIN project_taxon pt ON pt.taxon_id = m.taxon_id "
    "WHERE m.status IN ('auto', 'confirmed')";

} // namespace

std::optional<QSet<QString>> PhotoSelection::notWanted(const QString &connection,
                                                       const QList<int> &tickedTrees,
                                                       QString *error)
{
    QStringList ids;
    for (const int id : tickedTrees)
        ids << QString::number(id);
    QString sql = QStringLiteral("SELECT r.path FROM rendition r WHERE r.capture_id IN (%1)")
                      .arg(QLatin1String(kInTrees));
    if (!ids.isEmpty())
        sql += QStringLiteral(" AND r.capture_id NOT IN (%1 AND pt.project_id IN (%2))")
                   .arg(QLatin1String(kInTrees), ids.join(QLatin1Char(',')));

    QSqlQuery q(QSqlDatabase::database(connection, false));
    q.setForwardOnly(true);
    if (!q.exec(sql)) {
        if (error)
            *error = q.lastError().text();
        return std::nullopt;
    }
    QSet<QString> out;
    while (q.next())
        out.insert(q.value(0).toString());
    return out;
}

QHash<QString, QString> PhotoSelection::contentHashes(const QString &connection)
{
    QHash<QString, QString> out;
    QSqlQuery q(QSqlDatabase::database(connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT path, content_hash FROM rendition WHERE content_hash IS NOT NULL")))
        return out;
    while (q.next())
        out.insert(q.value(0).toString(), q.value(1).toString());
    return out;
}

QList<PhotoSelection::TreeStats> PhotoSelection::treeStats(const QString &connection)
{
    QList<TreeStats> out;
    QSqlQuery q(QSqlDatabase::database(connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT p.id, p.name, "
            " (SELECT COUNT(DISTINCT m.capture_id) FROM capture_match m"
            "  JOIN project_taxon pt ON pt.taxon_id = m.taxon_id"
            "  WHERE pt.project_id = p.id AND m.status IN ('auto', 'confirmed')),"
            " (SELECT COALESCE(SUM(r.file_size), 0) FROM rendition r WHERE r.capture_id IN"
            "  (SELECT m.capture_id FROM capture_match m"
            "   JOIN project_taxon pt ON pt.taxon_id = m.taxon_id"
            "   WHERE pt.project_id = p.id AND m.status IN ('auto', 'confirmed')))"
            " FROM project p ORDER BY p.name COLLATE NOCASE")))
        return out;
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString(), q.value(2).toInt(),
                     q.value(3).toLongLong()});
    return out;
}

} // namespace pl::libsync
