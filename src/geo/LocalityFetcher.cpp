#include "geo/LocalityFetcher.h"

#include "geo/LocalityStore.h"
#include "net/GeocodeClient.h"

namespace pl::geo {

LocalityFetcher::LocalityFetcher(pl::net::GeocodeClient &geocoder, QString connectionName,
                                 QObject *parent)
    : QObject(parent), m_geocoder(geocoder), m_connectionName(std::move(connectionName))
{
}

void LocalityFetcher::start()
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_index = 0;
    m_done = 0;
    m_updated = 0;

    m_pending = LocalityStore(m_connectionName).coordinatesNeedingLookup();
    emit progress(0, int(m_pending.size()));
    fetchNext();
}

bool LocalityFetcher::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        m_running = false;
        emit finished(false, QStringLiteral("cancelled"), m_updated);
        return true;
    }
    return false;
}

void LocalityFetcher::fetchNext()
{
    if (checkCancelled())
        return;
    if (m_index >= m_pending.size()) {
        succeed();
        return;
    }

    const QPair<double, double> coord = m_pending.at(m_index);
    m_geocoder.reverseGeocode(coord.first, coord.second,
                              [this, coord](pl::net::Outcome<QString> out) {
        if (checkCancelled())
            return;

        if (out.ok()
            && LocalityStore(m_connectionName).upsertLocality(coord.first, coord.second, out.value))
            ++m_updated;

        ++m_done;
        ++m_index;
        emit progress(m_done, int(m_pending.size()));
        fetchNext();
    });
}

void LocalityFetcher::succeed()
{
    m_running = false;
    emit finished(true, QString(), m_updated);
}

} // namespace pl::geo
