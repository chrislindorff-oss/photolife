#include "net/GeocodeClient.h"

#include "net/GeocodeParse.h"
#include "net/HttpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace pl::net {

GeocodeClient::GeocodeClient(HttpClient &http, QObject *parent)
    : QObject(parent), m_http(http)
{
}

void GeocodeClient::reverseGeocode(double lat, double lon,
                                   std::function<void(Outcome<QString>)> done)
{
    QUrl url(QStringLiteral("https://nominatim.openstreetmap.org/reverse"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("lat"), QString::number(lat, 'f', 6));
    query.addQueryItem(QStringLiteral("lon"), QString::number(lon, 'f', 6));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("jsonv2"));
    query.addQueryItem(QStringLiteral("addressdetails"), QStringLiteral("1"));
    url.setQuery(query);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QString> out;
        if (!resp.error.isEmpty()) {
            out.error = resp.error;
            done(out);
            return;
        }
        if (!resp.ok()) {
            out.error = QStringLiteral("HTTP %1").arg(resp.status);
            done(out);
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &parseError);
        if (doc.isNull() || !doc.isObject()) {
            out.error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
            done(out);
            return;
        }
        out.value = geocode::formatLocality(doc.object());
        done(out);
    });
}

void GeocodeClient::search(const QString &query,
                           std::function<void(Outcome<QList<geocode::Place>>)> done)
{
    QUrl url(QStringLiteral("https://nominatim.openstreetmap.org/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("jsonv2"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("5"));
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QList<geocode::Place>> out;
        if (!resp.error.isEmpty() || !resp.ok()) {
            out.error = resp.error.isEmpty() ? QStringLiteral("HTTP %1").arg(resp.status) : resp.error;
            done(out);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(resp.body);
        if (!doc.isArray()) {
            out.error = QStringLiteral("invalid JSON");
            done(out);
            return;
        }
        out.value = geocode::parseSearch(doc.array());
        done(out);
    });
}

} // namespace pl::net
