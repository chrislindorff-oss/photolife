#include "inat/InatObservationFetcher.h"

#include "net/INatClient.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtMath>

#include <cmath>

namespace pl::inat {
namespace {

double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusKm = 6371.0;
    const double dLat = qDegreesToRadians(lat2 - lat1);
    const double dLon = qDegreesToRadians(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                    + std::cos(qDegreesToRadians(lat1)) * std::cos(qDegreesToRadians(lat2))
                          * std::sin(dLon / 2) * std::sin(dLon / 2);
    return kEarthRadiusKm * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

} // namespace

InatObservationFetcher::InatObservationFetcher(net::INatClient &inat, QString connectionName,
                                               QObject *parent)
    : QObject(parent), m_inat(inat), m_connectionName(std::move(connectionName))
{
}

void InatObservationFetcher::loadLocalRecords()
{
    m_localRecords.clear();

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.setForwardOnly(true);
    q.exec(QStringLiteral(
        "SELECT t.inat_id, c.captured_on, c.latitude, c.longitude FROM capture c "
        "JOIN capture_match m ON m.id = ("
        "  SELECT id FROM capture_match WHERE capture_id = c.id "
        "  ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "JOIN taxon t ON t.id = m.taxon_id "
        "WHERE c.captured_on IS NOT NULL"));

    while (q.next()) {
        LocalRecord r;
        r.taxonInatId = q.value(0).toLongLong();
        r.date = QDate::fromString(q.value(1).toString().left(10), Qt::ISODate);
        if (!r.date.isValid())
            continue;
        if (!q.value(2).isNull() && !q.value(3).isNull()) {
            r.latitude = q.value(2).toDouble();
            r.longitude = q.value(3).toDouble();
        }
        m_localRecords.append(r);
    }

    // Exact photo-id matches, independent of the taxon+date+GPS heuristic
    // above -- this also catches photos downloaded but not yet reviewed/
    // matched to a taxon, which the heuristic query above misses entirely
    // (it requires a capture_match row).
    m_downloadedPhotoIds.clear();
    QSqlQuery photoIds(QSqlDatabase::database(m_connectionName, false));
    photoIds.setForwardOnly(true);
    photoIds.exec(QStringLiteral("SELECT inat_photo_id FROM capture WHERE inat_photo_id IS NOT NULL"));
    while (photoIds.next())
        m_downloadedPhotoIds.insert(photoIds.value(0).toLongLong());
}

bool InatObservationFetcher::looksLikeDuplicate(const taxonomy::Observation &obs) const
{
    if (obs.taxonInatId <= 0 || obs.observedOn.isEmpty())
        return false;
    const QDate obsDate = QDate::fromString(obs.observedOn.left(10), Qt::ISODate);
    if (!obsDate.isValid())
        return false;

    for (const LocalRecord &r : m_localRecords) {
        if (r.taxonInatId != obs.taxonInatId)
            continue;
        if (std::abs(r.date.daysTo(obsDate)) > 1)
            continue;
        if (obs.latitude && obs.longitude && r.latitude && r.longitude
            && haversineKm(*r.latitude, *r.longitude, *obs.latitude, *obs.longitude)
                   > kDuplicateRadiusKm)
            continue;
        return true;
    }
    return false;
}

QSet<qint64> InatObservationFetcher::alreadyDownloadedPhotoIds(const taxonomy::Observation &obs) const
{
    QSet<qint64> ids;
    for (const taxonomy::ObservationPhoto &photo : obs.photos) {
        if (m_downloadedPhotoIds.contains(photo.id))
            ids.insert(photo.id);
    }
    return ids;
}

void InatObservationFetcher::start(const QString &userLogin, const QList<qint64> &taxonIds,
                                   qint64 placeId)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_userLogin = userLogin;
    m_placeId = placeId;
    m_batchIndex = 0;
    m_candidates.clear();

    m_batches.clear();
    for (int i = 0; i < taxonIds.size(); i += kBatchSize)
        m_batches.append(taxonIds.mid(i, kBatchSize));

    loadLocalRecords();
    emit progress(0, m_batches.size(), 0);
    fetchNextBatch();
}

bool InatObservationFetcher::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void InatObservationFetcher::fetchNextBatch()
{
    if (checkCancelled())
        return;
    if (m_batchIndex >= m_batches.size()) {
        succeed();
        return;
    }
    m_page = 1;
    m_batchFetchedCount = 0;
    fetchPage();
}

void InatObservationFetcher::fetchPage()
{
    if (checkCancelled())
        return;

    m_inat.fetchObservations(
        m_userLogin, m_batches.at(m_batchIndex), m_placeId, m_page,
        [this](net::Outcome<net::ObservationPage> out) {
            if (checkCancelled())
                return;
            if (!out.ok()) {
                fail(out.error);
                return;
            }

            for (const taxonomy::Observation &obs : out.value.results)
                m_candidates.append({obs, looksLikeDuplicate(obs), alreadyDownloadedPhotoIds(obs)});
            m_batchFetchedCount += out.value.results.size();
            emit progress(m_batchIndex, m_batches.size(), m_candidates.size());

            if (m_batchFetchedCount < out.value.totalResults && !out.value.results.isEmpty()) {
                ++m_page;
                fetchPage();
                return;
            }

            ++m_batchIndex;
            fetchNextBatch();
        });
}

void InatObservationFetcher::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, {});
}

void InatObservationFetcher::succeed()
{
    m_running = false;
    emit finished(true, QString(), m_candidates);
}

} // namespace pl::inat
