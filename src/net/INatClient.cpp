#include "net/INatClient.h"

#include "net/HttpClient.h"
#include "net/INatParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

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

// The JSON object a POST answers with, or sets `error` -- with what
// iNaturalist said was wrong, when it said (a 422 names the field).
QJsonObject postedObject(const HttpResponse &resp, QString *error)
{
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body);
    if (!resp.error.isEmpty()) {
        *error = resp.error;
        return {};
    }
    if (!resp.ok()) {
        const QJsonValue said = doc.object().value(QStringLiteral("error"));
        QString why = said.isObject() ? said.toObject().value(QStringLiteral("original")).toObject()
                                            .value(QStringLiteral("error")).toString()
                                      : said.toString();
        if (why.isEmpty() && said.isObject())
            why = QString::fromUtf8(QJsonDocument(said.toObject()).toJson(QJsonDocument::Compact));
        *error = why.isEmpty() ? QStringLiteral("HTTP %1").arg(resp.status)
                               : QStringLiteral("HTTP %1: %2").arg(resp.status).arg(why.left(300));
        return {};
    }
    // An object, or the object in an array or a "results" list.
    QJsonObject obj = doc.isArray() ? doc.array().first().toObject() : doc.object();
    if (obj.contains(QStringLiteral("results")))
        obj = obj.value(QStringLiteral("results")).toArray().first().toObject();
    if (obj.isEmpty())
        *error = QStringLiteral("an unexpected answer from iNaturalist");
    return obj;
}

// multipart/form-data: the `fields` in order, then the JPEG as `fileField`.
// Sets `contentType` to go with it.
QByteArray multipartBody(const QList<std::pair<QByteArray, QByteArray>> &fields, const QByteArray &fileField,
                         const QString &fileName, const QByteArray &jpeg, QByteArray *contentType)
{
    const QByteArray boundary = "photolife-" + QUuid::createUuid().toByteArray(QUuid::Id128);
    QByteArray body;
    for (const auto &[name, value] : fields)
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n"
                + value + "\r\n";
    QByteArray safeName = fileName.toUtf8();
    safeName.replace('"', '_');
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fileField + "\"; filename=\""
            + safeName + "\"\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + "\r\n--" + boundary + "--\r\n";
    *contentType = "multipart/form-data; boundary=" + boundary;
    return body;
}

} // namespace

INatClient::INatClient(HttpClient &http, QObject *parent)
    : QObject(parent), m_http(http)
{
}

void INatClient::request(const QUrl &url, HttpClient::Handler handler, const QString &bearerToken)
{
    m_http.get(url, std::move(handler), bearerToken, m_priority);
}

void INatClient::scoreImage(const QByteArray &jpeg, std::optional<QPointF> lonLat,
                            std::optional<QDate> observedOn,
                            std::function<void(Outcome<VisionResult>)> done)
{
    QList<std::pair<QByteArray, QByteArray>> fields;
    if (lonLat) {
        fields.append({"lat", QByteArray::number(lonLat->y(), 'f', 6)});
        fields.append({"lng", QByteArray::number(lonLat->x(), 'f', 6)});
    }
    if (observedOn && observedOn->isValid())
        fields.append({"observed_on", observedOn->toString(Qt::ISODate).toLatin1()});
    QByteArray contentType;
    const QByteArray body = multipartBody(fields, "image", QStringLiteral("photo.jpg"), jpeg, &contentType);

    auto suggestion = [](const QJsonObject &obj) {
        VisionSuggestion s;
        s.taxon = inat::parseTaxon(obj.value(QStringLiteral("taxon")).toObject());
        s.score = obj.value(QStringLiteral("combined_score")).toDouble(obj.value(QStringLiteral("score")).toDouble());
        s.visionScore = obj.value(QStringLiteral("vision_score")).toDouble();
        s.seenNearby = obj.value(QStringLiteral("frequency_score")).toDouble() > 0;
        return s;
    };
    m_http.post(QUrl(m_baseUrl + QStringLiteral("/computervision/score_image")), body, contentType,
                [done = std::move(done), suggestion](HttpResponse resp) {
                    Outcome<VisionResult> out;
                    out.status = resp.status;
                    const QJsonArray results = resultsArray(resp, &out.error);
                    if (!out.ok()) {
                        done(out);
                        return;
                    }
                    for (const QJsonValue &v : results)
                        out.value.suggestions.append(suggestion(v.toObject()));
                    const QJsonObject ancestor =
                        QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("common_ancestor")).toObject();
                    if (!ancestor.value(QStringLiteral("taxon")).toObject().isEmpty())
                        out.value.commonAncestor = suggestion(ancestor);
                    done(out);
                },
                m_accessToken, m_priority);
}

void INatClient::fetchMe(std::function<void(Outcome<QString>)> done)
{
    request(QUrl(m_baseUrl + QStringLiteral("/users/me")),
            [done = std::move(done)](HttpResponse resp) {
                Outcome<QString> out;
                out.status = resp.status;
                const QJsonArray results = resultsArray(resp, &out.error);
                if (out.ok()) {
                    out.value = results.first().toObject().value(QStringLiteral("login")).toString();
                    if (out.value.isEmpty())
                        out.error = QStringLiteral("iNaturalist didn't say who this token belongs to");
                }
                done(out);
            },
            m_accessToken);
}

void INatClient::createObservation(const NewObservation &observation, std::function<void(Outcome<qint64>)> done)
{
    QJsonObject o;
    o.insert(QStringLiteral("uuid"), observation.uuid);
    if (observation.taxonId > 0)
        o.insert(QStringLiteral("taxon_id"), observation.taxonId);
    if (!observation.speciesGuess.isEmpty())
        o.insert(QStringLiteral("species_guess"), observation.speciesGuess);
    QString when = observation.date.toString(Qt::ISODate);
    if (observation.time)
        when += QLatin1Char(' ') + observation.time->toString(QStringLiteral("HH:mm:ss"));
    o.insert(QStringLiteral("observed_on_string"), when);
    o.insert(QStringLiteral("latitude"), observation.latitude);
    o.insert(QStringLiteral("longitude"), observation.longitude);
    if (!observation.geoprivacy.isEmpty())
        o.insert(QStringLiteral("geoprivacy"), observation.geoprivacy);
    if (!observation.description.isEmpty())
        o.insert(QStringLiteral("description"), observation.description);
    o.insert(QStringLiteral("owners_identification_from_vision"), observation.fromVision);
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("observation"), o}}).toJson(QJsonDocument::Compact);

    m_http.post(QUrl(m_baseUrl + QStringLiteral("/observations")), body, "application/json",
                [done = std::move(done)](HttpResponse resp) {
                    Outcome<qint64> out;
                    out.status = resp.status;
                    const QJsonObject obj = postedObject(resp, &out.error);
                    if (out.ok()) {
                        out.value = obj.value(QStringLiteral("id")).toInteger();
                        if (out.value <= 0)
                            out.error = QStringLiteral("iNaturalist didn't give the new observation's id");
                    }
                    done(out);
                },
                m_accessToken, m_priority, HttpClient::Retry::OnlyIfRefused);
}

void INatClient::findObservationByUuid(const QString &uuid,
                                       std::function<void(Outcome<std::optional<qint64>>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("uuid"), uuid);
    url.setQuery(q);
    request(url,
            [done = std::move(done)](HttpResponse resp) {
                Outcome<std::optional<qint64>> out;
                out.status = resp.status;
                const QJsonArray results = resultsArray(resp, &out.error);
                if (out.ok() && !results.isEmpty())
                    out.value = results.first().toObject().value(QStringLiteral("id")).toInteger();
                done(out);
            },
            m_accessToken);
}

void INatClient::addObservationPhoto(qint64 observationId, const QByteArray &jpeg, const QString &fileName,
                                     std::function<void(Outcome<qint64>)> done)
{
    QByteArray contentType;
    const QByteArray body = multipartBody({{"observation_photo[observation_id]", QByteArray::number(observationId)}},
                                          "file", fileName, jpeg, &contentType);
    m_http.post(QUrl(m_baseUrl + QStringLiteral("/observation_photos")), body, contentType,
                [done = std::move(done)](HttpResponse resp) {
                    Outcome<qint64> out;
                    out.status = resp.status;
                    const QJsonObject obj = postedObject(resp, &out.error);
                    if (out.ok()) {
                        out.value = obj.value(QStringLiteral("photo")).toObject().value(QStringLiteral("id")).toInteger();
                        if (out.value <= 0)
                            out.value = obj.value(QStringLiteral("photo_id")).toInteger();
                        if (out.value <= 0)
                            out.error = QStringLiteral("iNaturalist didn't give the new photo's id");
                    }
                    done(out);
                },
                m_accessToken, m_priority, HttpClient::Retry::OnlyIfRefused);
}

void INatClient::resolvePlaces(const QString &query,
                               std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/places/autocomplete"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    url.setQuery(q);

    request(url, [done = std::move(done)](HttpResponse resp) {
        Outcome<QList<pl::taxonomy::Place>> out;
        const QJsonArray results = resultsArray(resp, &out.error);
        if (out.ok()) {
            for (const QJsonValue &v : results)
                out.value.append(inat::parsePlace(v.toObject()));
        }
        done(out);
    });
}

void INatClient::fetchPlaces(const QList<qint64> &inatIds,
                             std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done)
{
    QStringList ids;
    for (qint64 id : inatIds)
        ids << QString::number(id);
    const QUrl url(m_baseUrl + QStringLiteral("/places/") + ids.join(QLatin1Char(',')));
    request(url, [done = std::move(done)](HttpResponse resp) {
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

    request(url, [done = std::move(done)](HttpResponse resp) {
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

    request(url, [done = std::move(done)](HttpResponse resp) {
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
                           std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done,
                           bool allNames)
{
    if (inatIds.isEmpty()) {
        done({});
        return;
    }
    QStringList idStrings;
    for (qint64 id : inatIds)
        idStrings << QString::number(id);

    QUrl url(m_baseUrl + QStringLiteral("/taxa/") + idStrings.join(QLatin1Char(',')));
    if (allNames)
        url.setQuery(QStringLiteral("all_names=true"));

    request(url, [done = std::move(done)](HttpResponse resp) {
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
    getSpeciesCountsPage(url, std::move(done));
}

void INatClient::userSpeciesCounts(const QString &userLogin, qint64 taxonId, qint64 placeId,
                                   ObservationQuality quality, int page, int perPage,
                                   std::function<void(Outcome<SpeciesCountsPage>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations/species_counts"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_login"), userLogin);
    if (taxonId > 0)
        q.addQueryItem(QStringLiteral("taxon_id"), QString::number(taxonId));
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    if (quality == ObservationQuality::Verifiable)
        q.addQueryItem(QStringLiteral("verifiable"), QStringLiteral("true"));
    else if (quality == ObservationQuality::Research)
        q.addQueryItem(QStringLiteral("quality_grade"), QStringLiteral("research"));
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("per_page"), QString::number(perPage));
    url.setQuery(q);
    getSpeciesCountsPage(url, std::move(done));
}

void INatClient::introducedSpeciesCounts(qint64 taxonId, qint64 placeId, int page, int perPage,
                                         std::function<void(Outcome<SpeciesCountsPage>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations/species_counts"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("taxon_id"), QString::number(taxonId));
    q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    q.addQueryItem(QStringLiteral("introduced"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("per_page"), QString::number(perPage));
    url.setQuery(q);
    getSpeciesCountsPage(url, std::move(done));
}

void INatClient::getSpeciesCountsPage(const QUrl &url,
                                      std::function<void(Outcome<SpeciesCountsPage>)> done)
{
    request(url, [done = std::move(done)](HttpResponse resp) {
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

    request(url, [done = std::move(done)](HttpResponse resp) {
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
                                   int page, std::function<void(Outcome<ObservationPage>)> done,
                                   bool photosOnly)
{
    QStringList ids;
    ids.reserve(taxonIds.size());
    for (qint64 id : taxonIds)
        ids << QString::number(id);

    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_login"), userLogin);
    if (!ids.isEmpty())   // none: every taxon
        q.addQueryItem(QStringLiteral("taxon_id"), ids.join(QLatin1Char(',')));
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    if (dateFrom.isValid())
        q.addQueryItem(QStringLiteral("d1"), dateFrom.toString(Qt::ISODate));
    if (dateTo.isValid())
        q.addQueryItem(QStringLiteral("d2"), dateTo.toString(Qt::ISODate));
    if (photosOnly)
        q.addQueryItem(QStringLiteral("photos"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("200"));
    url.setQuery(q);
    getObservationPage(url, std::move(done));
}

void INatClient::fetchObservationsById(const QList<qint64> &observationIds,
                                       std::function<void(Outcome<ObservationPage>)> done)
{
    QStringList ids;
    for (qint64 id : observationIds.mid(0, 200))
        ids << QString::number(id);
    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("id"), ids.join(QLatin1Char(',')));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("200"));
    url.setQuery(q);
    getObservationPage(url, std::move(done));
}

void INatClient::fetchAllObservations(const QString &userLogin, qint64 placeId, qint64 taxonId,
                                      qint64 idAbove,
                                      std::function<void(Outcome<ObservationPage>)> done)
{
    QUrl url(m_baseUrl + QStringLiteral("/observations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_login"), userLogin);
    if (placeId > 0)
        q.addQueryItem(QStringLiteral("place_id"), QString::number(placeId));
    if (taxonId > 0)
        q.addQueryItem(QStringLiteral("taxon_id"), QString::number(taxonId));
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
    request(
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
