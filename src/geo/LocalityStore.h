#pragma once

#include <QList>
#include <QPair>
#include <QString>

namespace pl::geo {

// The geocode_cache table: reverse-geocoded localities keyed by GPS
// coordinates rounded to 3 decimal places (~100m), so nearby captures share
// one lookup. Nothing here touches the network. Give it an open connection
// name; all calls run on the caller's thread.
class LocalityStore
{
public:
    explicit LocalityStore(QString connectionName);

    // Rounds (lat, lon) and upserts the result. `locality` may be empty --
    // that's a real result meaning nothing was found for that point, so it
    // isn't looked up again. Returns false on error (see error()).
    bool upsertLocality(double lat, double lon, const QString &locality);

    // Distinct (rounded lat, rounded lon) pairs among captures with GPS that
    // have no cache entry yet -- the fetch work list, one entry per distinct
    // location rather than per photo.
    QList<QPair<double, double>> coordinatesNeedingLookup() const;

    QString error() const { return m_error; }

private:
    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::geo
