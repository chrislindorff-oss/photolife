#pragma once

#include <QList>
#include <QPair>
#include <QString>

#include <optional>

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

    // The cached locality for (lat, lon), rounded the same way; nullopt when
    // it hasn't been looked up (an empty string means "nothing there").
    // Places cached with no town before kLocalityKeysSince count as not
    // looked up, so they're fetched once more, now with the locality.
    std::optional<QString> lookup(double lat, double lon) const;

    // When the geocoder began reading OpenStreetMap's locality keys
    // ("locality", "city_district"), which name places away from towns.
    static constexpr const char *kLocalityKeysSince = "2026-10-31T02:45:00Z";

    // Distinct (rounded lat, rounded lon) pairs among captures with GPS that
    // have no cache entry yet -- the fetch work list, one entry per distinct
    // location rather than per photo.
    QList<QPair<double, double>> coordinatesNeedingLookup() const;

    QString error() const { return m_error; }

private:
    // SQL: the row of `table` is a town-less place from before kLocalityKeysSince.
    static QString staleCondition(const QString &table);

    QString m_connectionName;
    mutable QString m_error;
};

} // namespace pl::geo
