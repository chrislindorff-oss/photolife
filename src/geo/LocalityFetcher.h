#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

namespace pl::net {
class GeocodeClient;
}

namespace pl::geo {

// Fills in geocode_cache for every distinct GPS location among the library's
// captures that doesn't have a locality yet. One Nominatim request per
// distinct location (not per photo); pacing comes from the GeocodeClient's
// own HttpClient, which throttles to Nominatim's usage policy.
//
// A coordinate whose lookup fails is simply left uncached rather than
// aborting the run -- it will be retried the next time this is started,
// instead of one bad request discarding progress on what can be a long,
// rate-limited batch.
//
// Drive it from the thread that owns the GeocodeClient / store connection.
class LocalityFetcher : public QObject
{
    Q_OBJECT

public:
    LocalityFetcher(pl::net::GeocodeClient &geocoder, QString connectionName,
                    QObject *parent = nullptr);

    void start();
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, int updated);

private:
    void fetchNext();
    bool checkCancelled();
    void succeed();

    pl::net::GeocodeClient &m_geocoder;
    QString m_connectionName;

    bool m_running = false;
    bool m_cancelled = false;
    QList<QPair<double, double>> m_pending;
    int m_index = 0;
    int m_done = 0;
    int m_updated = 0;
};

} // namespace pl::geo
