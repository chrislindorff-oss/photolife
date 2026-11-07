#include "catalogue/SeenStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::catalogue {

SeenStore::SeenStore(QString connectionName) : m_connectionName(std::move(connectionName)) {}

QSet<qint64> SeenStore::seenTaxa() const
{
    m_error.clear();
    QSet<qint64> out;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT inat_id FROM taxon_seen"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next())
        out.insert(q.value(0).toLongLong());
    return out;
}

bool SeenStore::setSeen(qint64 inatId, bool seen)
{
    m_error.clear();
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(seen ? QStringLiteral("INSERT INTO taxon_seen (inat_id) VALUES (?) ON CONFLICT(inat_id) DO NOTHING")
                   : QStringLiteral("DELETE FROM taxon_seen WHERE inat_id = ?"));
    q.addBindValue(inatId);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

} // namespace pl::catalogue
