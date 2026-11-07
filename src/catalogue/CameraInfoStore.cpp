#include "catalogue/CameraInfoStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::catalogue {

CameraInfoStore::CameraInfoStore(QString connectionName) : m_connectionName(std::move(connectionName)) {}

bool CameraInfoStore::save(const QList<CameraInfo> &infos)
{
    m_error.clear();
    if (infos.isEmpty())
        return true;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.transaction()) {
        m_error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO capture_camera (capture_id, camera, focal_length) VALUES (?, ?, ?) "
                             "ON CONFLICT(capture_id) DO UPDATE SET camera = excluded.camera, "
                             "focal_length = excluded.focal_length"));
    for (const CameraInfo &info : infos) {
        q.bindValue(0, info.captureId);
        q.bindValue(1, info.camera.isEmpty() ? QVariant() : QVariant(info.camera));
        q.bindValue(2, info.focalLength ? QVariant(*info.focalLength) : QVariant());
        if (!q.exec()) {
            m_error = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        m_error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

} // namespace pl::catalogue
