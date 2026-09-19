#include "scan/CatalogueMaintenance.h"

#include "raw/RawPreview.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace pl::scan {

CatalogueMaintenance::CatalogueMaintenance(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

int CatalogueMaintenance::forgetCaptures(const QList<int> &captureIds)
{
    m_error.clear();
    if (captureIds.isEmpty())
        return 0;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        m_error = QStringLiteral("catalogue connection is not open");
        return 0;
    }
    if (!db.transaction()) {
        m_error = db.lastError().text();
        return 0;
    }

    QStringList marks;
    marks.reserve(captureIds.size());
    for (int i = 0; i < captureIds.size(); ++i)
        marks << QStringLiteral("?");

    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM capture WHERE id IN (%1)")
                  .arg(marks.join(QLatin1Char(','))));
    for (int id : captureIds)
        q.addBindValue(id);

    if (!q.exec()) {
        m_error = q.lastError().text();
        db.rollback();
        return 0;
    }
    const int removed = q.numRowsAffected();

    if (!db.commit()) {
        m_error = db.lastError().text();
        return 0;
    }
    return removed;
}

int CatalogueMaintenance::captureCountUnderFolder(const QString &rootPath) const
{
    m_error.clear();
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "WITH RECURSIVE sub(id) AS ("
        "  SELECT id FROM folder WHERE path = ?"
        "  UNION ALL"
        "  SELECT f.id FROM folder f JOIN sub ON f.parent_id = sub.id"
        ") "
        "SELECT COUNT(*) FROM capture WHERE folder_id IN (SELECT id FROM sub)"));
    q.addBindValue(rootPath);
    if (!q.exec() || !q.next()) {
        m_error = q.lastError().text();
        return 0;
    }
    return q.value(0).toInt();
}

int CatalogueMaintenance::purgeFolder(const QString &rootPath)
{
    m_error.clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        m_error = QStringLiteral("catalogue connection is not open");
        return -1;
    }

    // Captures are about to vanish via cascade, so count them first --
    // numRowsAffected() on the folder delete only counts folder rows.
    const int captureCount = captureCountUnderFolder(rootPath);
    if (!m_error.isEmpty())
        return -1;

    if (!db.transaction()) {
        m_error = db.lastError().text();
        return -1;
    }

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM folder WHERE path = ?"));
    del.addBindValue(rootPath);
    if (!del.exec()) {
        m_error = del.lastError().text();
        db.rollback();
        return -1;
    }
    const int foldersRemoved = del.numRowsAffected();

    if (!db.commit()) {
        m_error = db.lastError().text();
        return -1;
    }
    return foldersRemoved > 0 ? captureCount : 0;
}

QList<CatalogueMaintenance::MissingCapture> CatalogueMaintenance::capturesWithMissingFiles() const
{
    m_error.clear();
    QList<MissingCapture> out;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT c.id, c.base_name, f.path, r.path FROM capture c "
            "JOIN folder f ON f.id = c.folder_id "
            "LEFT JOIN rendition r ON r.capture_id = c.id "
            "ORDER BY f.path, c.base_name, c.id"))) {
        m_error = q.lastError().text();
        return out;
    }

    int currentId = -1;
    bool anyPresent = false;
    MissingCapture pending;
    auto flush = [&] {
        if (currentId > 0 && !anyPresent)
            out.append(pending);
    };

    while (q.next()) {
        const int id = q.value(0).toInt();
        if (id != currentId) {
            flush();
            currentId = id;
            anyPresent = false;
            pending = MissingCapture{};
            pending.captureId = id;
            pending.baseName = q.value(1).toString();
            pending.folderPath = q.value(2).toString();
        }
        const QString path = q.value(3).toString();
        if (path.isEmpty())
            continue;
        if (QFileInfo::exists(path))
            anyPresent = true;
        else
            pending.missingPaths.append(path);
    }
    flush();

    return out;
}

int CatalogueMaintenance::backfillRawGeolocation()
{
    m_error.clear();

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT capture_id, path FROM rendition WHERE kind = 'raw'"))) {
        m_error = q.lastError().text();
        return 0;
    }

    QList<QPair<int, QString>> rawRenditions;
    while (q.next())
        rawRenditions.append({q.value(0).toInt(), q.value(1).toString()});

    int updated = 0;
    for (int i = 0; i < rawRenditions.size(); ++i) {
        const int captureId = rawRenditions.at(i).first;
        const raw::RawGps gps = raw::extractGps(rawRenditions.at(i).second);

        QSqlQuery upd(db);
        if (gps.latitude && gps.longitude) {
            upd.prepare(
                QStringLiteral("UPDATE capture SET latitude = ?, longitude = ? WHERE id = ?"));
            upd.addBindValue(*gps.latitude);
            upd.addBindValue(*gps.longitude);
            upd.addBindValue(captureId);
        } else {
            // No trustworthy GPS on this RAW file. Clear a (0, 0) "Null Island"
            // value only if an earlier, buggier version of this backfill wrote
            // it (LibRaw's gpsparsed flag can be set with no real fix); leave
            // any other stored value -- e.g. from a sibling JPEG rendition --
            // alone.
            upd.prepare(QStringLiteral(
                "UPDATE capture SET latitude = NULL, longitude = NULL "
                "WHERE id = ? AND latitude = 0 AND longitude = 0"));
            upd.addBindValue(captureId);
        }
        if (upd.exec() && upd.numRowsAffected() > 0)
            ++updated;

        if (i % 50 == 0)
            QCoreApplication::processEvents();
    }
    return updated;
}

} // namespace pl::scan
