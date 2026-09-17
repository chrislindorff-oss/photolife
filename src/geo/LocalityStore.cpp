#include "geo/LocalityStore.h"

#include "db/Database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>

namespace pl::geo {
namespace {

constexpr double kRoundTo = 1000.0;   // 3 decimal places, ~100m

double rounded(double value)
{
    return std::round(value * kRoundTo) / kRoundTo;
}

} // namespace

LocalityStore::LocalityStore(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

bool LocalityStore::upsertLocality(double lat, double lon, const QString &locality)
{
    m_error.clear();

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "INSERT INTO geocode_cache (lat_round, lon_round, locality) VALUES (?, ?, ?) "
        "ON CONFLICT(lat_round, lon_round) DO UPDATE SET locality = excluded.locality, "
        "  fetched_at = %1")
                  .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
    q.addBindValue(rounded(lat));
    q.addBindValue(rounded(lon));
    q.addBindValue(locality);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

QList<QPair<double, double>> LocalityStore::coordinatesNeedingLookup() const
{
    m_error.clear();
    QList<QPair<double, double>> out;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.setForwardOnly(true);
    // Two-argument ROUND() needs a NUMERIC operand on Postgres (it has no
    // overload for a bare double precision/real column), and the explicit
    // cast works identically on SQLite.
    if (!q.exec(QStringLiteral(
            "SELECT DISTINCT ROUND(CAST(latitude AS NUMERIC), 3), ROUND(CAST(longitude AS NUMERIC), 3) "
            "FROM capture "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "AND NOT EXISTS (SELECT 1 FROM geocode_cache g "
            "  WHERE g.lat_round = ROUND(CAST(capture.latitude AS NUMERIC), 3) "
            "    AND g.lon_round = ROUND(CAST(capture.longitude AS NUMERIC), 3))"))) {
        m_error = q.lastError().text();
        return out;
    }

    while (q.next())
        out.append({q.value(0).toDouble(), q.value(1).toDouble()});
    return out;
}

} // namespace pl::geo
