#include "catalogue/UntreedMatchStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::catalogue {

UntreedMatchStore::UntreedMatchStore(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

QList<UntreedTaxon> UntreedMatchStore::untreedTaxa(int projectId) const
{
    if (projectId > 0)
        return matchedTaxa(QStringLiteral("NOT EXISTS (SELECT 1 FROM project_taxon pt "
                                          "WHERE pt.taxon_id = t.id AND pt.project_id = ?)"),
                           {projectId});
    return matchedTaxa(
        QStringLiteral("NOT EXISTS (SELECT 1 FROM project_taxon pt WHERE pt.taxon_id = t.id)"), {});
}

QList<UntreedTaxon> UntreedMatchStore::aboveSpeciesTaxa() const
{
    // rank_level 10 is species; infraspecific ranks sit below it.
    return matchedTaxa(QStringLiteral("t.rank_level > 10"), {});
}

QList<UntreedTaxon> UntreedMatchStore::matchedTaxa(const QString &condition,
                                                   const QVariantList &binds) const
{
    m_error.clear();
    QList<UntreedTaxon> result;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT t.inat_id, t.name, t.common_name, t.rank, t.rank_level, COUNT(*), "
        "       SUM(CASE WHEN m.status = 'confirmed' THEN 1 ELSE 0 END) "
        "FROM capture c "
        "JOIN capture_match m ON m.id = ("
        "   SELECT id FROM capture_match WHERE capture_id = c.id "
        "   ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "JOIN taxon t ON t.id = m.taxon_id "
        "WHERE m.status IN ('auto', 'confirmed') AND %1 "
        "GROUP BY t.inat_id, t.name, t.common_name, t.rank, t.rank_level "
        "ORDER BY t.name")
                  .arg(condition));
    for (const QVariant &v : binds)
        q.addBindValue(v);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return result;
    }

    while (q.next()) {
        UntreedTaxon t;
        t.inatId = q.value(0).toLongLong();
        t.name = q.value(1).toString();
        t.commonName = q.value(2).toString();
        t.rank = q.value(3).toString();
        t.rankLevel = q.value(4).toInt();
        t.photoCount = q.value(5).toInt();
        t.confirmedCount = q.value(6).toInt();
        result << t;
    }
    return result;
}

} // namespace pl::catalogue
