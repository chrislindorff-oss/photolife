#include "catalogue/BestShotStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::catalogue {

BestShotStore::BestShotStore(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

int BestShotStore::setBestShots(const QList<int> &captureIds, bool nominate)
{
    m_error.clear();
    if (captureIds.isEmpty())
        return 0;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.transaction()) {
        m_error = db.lastError().text();
        return -1;
    }

    QSqlQuery q(db);
    q.prepare(nominate
                  ? QStringLiteral("INSERT INTO best_shot (capture_id) VALUES (?) "
                                   "ON CONFLICT(capture_id) DO NOTHING")
                  : QStringLiteral("DELETE FROM best_shot WHERE capture_id = ?"));

    int changed = 0;
    for (int id : captureIds) {
        q.bindValue(0, id);
        if (!q.exec()) {
            m_error = q.lastError().text();
            db.rollback();
            return -1;
        }
        changed += q.numRowsAffected();
    }

    if (!db.commit()) {
        m_error = db.lastError().text();
        db.rollback();
        return -1;
    }
    return changed;
}

bool BestShotStore::isBestShot(int captureId) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("SELECT 1 FROM best_shot WHERE capture_id = ?"));
    q.addBindValue(captureId);
    return q.exec() && q.next();
}

} // namespace pl::catalogue
