#include "net/INatClient.h"

#include "net/HttpClient.h"
#include "net/INatParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace pl::net {
namespace {

// Parses `body` as a JSON object and hands back its "results" array, or sets
// `error` describing why it could not.
QJsonArray resultsArray(const HttpResponse &resp, QString *error)
{
    if (!resp.error.isEmpty()) {
        *error = resp.error;
        return {};
    }
    if (!resp.ok()) {
        *error = QStringLiteral("HTTP %1").arg(resp.status);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        *error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
        return {};
    }
    return doc.object().value(QStringLiteral("results")).toArray();
}

} // namespace

INatClient::INatClient(HttpClient &http, QObject *parent)
    : QObject(parent), m_http(http)
{
}

void INatClient::resolvePlaces(const QString &query,
                               std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/places/autocomplete"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QList<pl::taxonomy::Place>> out;
        const QJsonArray results = resultsArray(resp, &out.error);
        if (out.ok()) {
            for (const QJsonValue &v : results)
                out.value.append(inat::parsePlace(v.toObject()));
        }
        done(out);
    });
}

void INatClient::searchTaxa(const QString &query, const QString &rank,
                            std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/taxa"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    if (!rank.isEmpty())
        q.addQueryItem(QStringLiteral("rank"), rank);
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QList<pl::taxonomy::Taxon>> out;
        const QJsonArray results = resultsArray(resp, &out.error);
        if (out.ok()) {
            for (const QJsonValue &v : results)
                out.value.append(inat::parseTaxon(v.toObject()));
        }
        done(out);
    });
}

void INatClient::fetchTaxon(qint64 inatId, std::function<void(Outcome<TaxonDetail>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/taxa/%1").arg(inatId));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("all_names"), QStringLiteral("true"));
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<TaxonDetail> out;
        const QJsonArray results = resultsArray(resp, &out.error);
        if (!out.ok()) {
            done(out);
            return;
        }
        if (results.isEmpty()) {
            out.error = QStringLiteral("taxon not found");
            done(out);
            return;
        }

        const QJsonObject obj = results.first().toObject();
        out.value.taxon = inat::parseTaxon(obj);
        for (const QJsonValue &v : obj.value(QStringLiteral("ancestors")).toArray())
            out.value.ancestors.append(inat::parseTaxon(v.toObject()));
        for (const QJsonValue &v : obj.value(QStringLiteral("children")).toArray())
            out.value.children.append(inat::parseTaxon(v.toObject()));
        done(out);
    });
}

void INatClient::fetchTaxa(const QList<qint64> &inatIds,
                           std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done)
{
    if (inatIds.isEmpty()) {
        done({});
        return;
    }
    QStringList idStrings;
    for (qint64 id : inatIds)
        idStrings << QString::number(id);

    QUrl url(m_baseUrl + QStringLiteral("/taxa/") + idStrings.join(QLatin1Char(',')));

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QList<pl::taxonomy::Taxon>> out;
        const QJsonArray results = resultsArray(resp, &out.error);
        if (out.ok()) {
            for (const QJsonValue &v : results)
                out.value.append(inat::parseTaxon(v.toObject()));
        }
        done(out);
    });
}

void INatClient::speciesCounts(qint64 taxonId, qint64 placeId, int page, int perPage,
                               std::function<void(Outcome<SpeciesCountsPage>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations/species_counts"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("taxon_id"), QString::number(taxonId));
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    q.addQueryItem(QStringLiteral("verifiable"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("per_page"), QString::number(perPage));
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<SpeciesCountsPage> out;
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
        if (!doc.isObject()) {
            out.error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
            done(out);
            return;
        }
        const QJsonObject root = doc.object();
        out.value.totalResults = root.value(QStringLiteral("total_results")).toInt();
        out.value.page = root.value(QStringLiteral("page")).toInt();
        out.value.perPage = root.value(QStringLiteral("per_page")).toInt();

        for (const QJsonValue &v : root.value(QStringLiteral("results")).toArray()) {
            const QJsonObject entry = v.toObject();
            SpeciesCount sc;
            sc.count = entry.value(QStringLiteral("count")).toInt();
            sc.taxon = inat::parseTaxon(entry.value(QStringLiteral("taxon")).toObject());
            sc.ancestorIds = inat::ancestryIds(sc.taxon.ancestry);
            out.value.results.append(sc);
        }
        done(out);
    });
}

void INatClient::observationCount(qint64 taxonId, qint64 placeId,
                                  std::function<void(Outcome<int>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("taxon_id"), QString::number(taxonId));
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    q.addQueryItem(QStringLiteral("verifiable"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("0"));
    url.setQuery(q);

    m_http.get(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<int> out;
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
        if (!doc.isObject()) {
            out.error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
            done(out);
            return;
        }
        out.value = doc.object().value(QStringLiteral("total_results")).toInt();
        done(out);
    });
}

void INatClient::fetchObservations(const QString &userLogin, const QList<qint64> &taxonIds,
                                   qint64 placeId, const QDate &dateFrom, const QDate &dateTo,
                                   int page, std::function<void(Outcome<ObservationPage>)> done)
{
    QStringList ids;
    ids.reserve(taxonIds.size());
    for (qint64 id : taxonIds)
        ids << QString::number(id);

    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_login"), userLogin);
    q.addQueryItem(QStringLiteral("taxon_id"), ids.join(QLatin1Char(',')));
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    if (dateFrom.isValid())
        q.addQueryItem(QStringLiteral("d1"), dateFrom.toString(Qt::ISODate));
    if (dateTo.isValid())
        q.addQueryItem(QStringLiteral("d2"), dateTo.toString(Qt::ISODate));
    q.addQueryItem(QStringLiteral("photos"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("200"));
    url.setQuery(q);
    getObservationPage(url, std::move(done));
}

void INatClient::fetchAllObservations(const QString &userLogin, qint64 placeId, qint64 idAbove,
                                      std::function<void(Outcome<ObservationPage>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_login"), userLogin);
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    q.addQueryItem(QStringLiteral("photos"), QStringLiteral("true"));
    // Keyset paging (id_above, ascending ids) rather than page=N: the API
    // refuses page-number paging past 10,000 results, which a whole
    // observation history can exceed.
    q.addQueryItem(QStringLiteral("order_by"), QStringLiteral("id"));
    q.addQueryItem(QStringLiteral("order"), QStringLiteral("asc"));
    if (idAbove > 0)
        q.addQueryItem(QStringLiteral("id_above"), QString::number(idAbove));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("200"));
    url.setQuery(q);
    getObservationPage(url, std::move(done));
}

void INatClient::getObservationPage(const QUrl &url,
                                    std::function<void(Outcome<ObservationPage>)> done)
{
    m_http.get(
        url,
        [done = std::move(done)](HttpResponse resp) {
            Outcome<ObservationPage> out;
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
            if (!doc.isObject()) {
                out.error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
                done(out);
                return;
            }
            const QJsonObject root = doc.object();
            out.value.totalResults = root.value(QStringLiteral("total_results")).toInt();
            for (const QJsonValue &v : root.value(QStringLiteral("results")).toArray())
                out.value.results.append(inat::parseObservation(v.toObject()));
            done(out);
        },
        m_accessToken);
}

} // namespace pl::net
