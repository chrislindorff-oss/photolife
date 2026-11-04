#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "FakeTransport.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"

using namespace pl;
using namespace pl::net;

class TestINatClient : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void resolvePlacesParsesResults();
    void searchTaxaPassesRankAndParses();
    void fetchTaxonReturnsAncestorsAndChildren();
    void speciesCountsParsesPageAndAncestry();
    void userSpeciesCountsSendsOnlyGivenFilters();
    void observationCountReadsTotalResults();
    void fetchObservationsSendsDateRangeWhenProvided();
    void fetchObservationsOmitsDateParamsWhenInvalid();
    void fetchObservationsWithNoTaxaSearchesEveryTaxon();
    void fetchObservationsCanIncludeThoseWithoutPhotos();
    void fetchObservationsByIdGivesThoseStillThere();
    void fetchAllObservationsSendsTaxonOnlyWhenGiven();
    void httpErrorBecomesOutcomeError();
    void invalidJsonBecomesOutcomeError();
    void scoreImagePostsThePhotoWithWhereAndWhen();
    void scoreImageReportsAnExpiredToken();

    void fetchMeGivesTheLogin();
    void createObservationPostsItsFields();
    void createObservationSaysWhatIsWrong();
    void findObservationByUuidTellsWhetherItExists();
    void addObservationPhotoPostsTheFile();

private:
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
    {
        auto owned = std::make_unique<FakeTransport>();
        owned->responder = std::move(responder);
        m_transport = owned.get();
        m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
        m_http->setMinRequestIntervalMs(0);
        m_http->setRetryBaseDelayMs(1);
        m_inat = std::make_unique<INatClient>(*m_http);
        m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    }
};

void TestINatClient::init()
{
    m_transport = nullptr;
    m_http.reset();
    m_inat.reset();
}

void TestINatClient::resolvePlacesParsesResults()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"results": [
            {"id": 6744, "name": "Victoria", "display_name": "Victoria, AU", "admin_level": 10},
            {"id": 111, "name": "Victoria Park", "admin_level": null}
        ]})");
    });

    Outcome<QList<taxonomy::Place>> got;
    bool done = false;
    m_inat->resolvePlaces(QStringLiteral("Victoria"), [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value.size(), 2);
    QCOMPARE(got.value.at(0).inatId, qint64(6744));
    QCOMPARE(got.value.at(0).displayName, QStringLiteral("Victoria, AU"));
    QVERIFY(m_transport->received.at(0).url.toString().contains(QStringLiteral("q=Victoria")));
}

void TestINatClient::searchTaxaPassesRankAndParses()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"results": [
            {"id": 83523, "rank": "genus", "name": "Diuris", "ancestry": "48460/47217"}
        ]})");
    });

    Outcome<QList<taxonomy::Taxon>> got;
    bool done = false;
    m_inat->searchTaxa(QStringLiteral("Diuris"), QStringLiteral("genus"),
                       [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value.size(), 1);
    QCOMPARE(got.value.at(0).name, QStringLiteral("Diuris"));
    QCOMPARE(got.value.at(0).parentInatId.value_or(-1), qint64(47217));
    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(url.contains(QStringLiteral("rank=genus")));
}

void TestINatClient::fetchTaxonReturnsAncestorsAndChildren()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"results": [{
            "id": 47217, "rank": "family", "name": "Orchidaceae",
            "ancestors": [
                {"id": 48460, "rank": "kingdom", "name": "Plantae"},
                {"id": 47163, "rank": "class", "name": "Magnoliopsida"}
            ],
            "children": [
                {"id": 500, "rank": "subfamily", "name": "Orchidoideae"}
            ]
        }]})");
    });

    Outcome<TaxonDetail> got;
    bool done = false;
    m_inat->fetchTaxon(47217, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value.taxon.name, QStringLiteral("Orchidaceae"));
    QCOMPARE(got.value.ancestors.size(), 2);
    QCOMPARE(got.value.ancestors.at(0).name, QStringLiteral("Plantae"));
    QCOMPARE(got.value.children.size(), 1);
    QVERIFY(m_transport->received.at(0).url.toString().contains(QStringLiteral("all_names=true")));
}

void TestINatClient::speciesCountsParsesPageAndAncestry()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({
            "total_results": 1344, "page": 1, "per_page": 2,
            "results": [
                {"count": 13193, "taxon": {"id": 83357, "rank": "species",
                 "name": "Glossodia major", "ancestry": "48460/47217/738348/752416"}},
                {"count": 11178, "taxon": {"id": 321216, "rank": "species",
                 "name": "Pterostylis nutans", "ancestry": "48460/47217/544791"}}
            ]
        })");
    });

    Outcome<SpeciesCountsPage> got;
    bool done = false;
    m_inat->speciesCounts(47217, 6744, 1, 2, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value.totalResults, 1344);
    QCOMPARE(got.value.results.size(), 2);
    QCOMPARE(got.value.results.at(0).count, 13193);
    QCOMPARE(got.value.results.at(0).taxon.name, QStringLiteral("Glossodia major"));
    QCOMPARE(got.value.results.at(0).ancestorIds.last(), qint64(752416));

    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(url.contains(QStringLiteral("taxon_id=47217")));
    QVERIFY(url.contains(QStringLiteral("place_id=6744")));
    QVERIFY(url.contains(QStringLiteral("verifiable=true")));
}

void TestINatClient::userSpeciesCountsSendsOnlyGivenFilters()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({
            "total_results": 1, "page": 1, "per_page": 500,
            "results": [
                {"count": 4, "taxon": {"id": 83357, "rank": "species",
                 "name": "Glossodia major", "ancestry": "48460/47217/738348/752416"}}
            ]
        })");
    });

    Outcome<SpeciesCountsPage> got;
    int calls = 0;
    m_inat->userSpeciesCounts(QStringLiteral("someone"), 47217, 6744, ObservationQuality::Research,
                              1, 500, [&](auto o) { got = o; ++calls; });
    QTRY_COMPARE(calls, 1);
    m_inat->userSpeciesCounts(QStringLiteral("someone"), 0, 0, ObservationQuality::Any, 2, 500,
                              [&](auto) { ++calls; });
    QTRY_COMPARE(calls, 2);

    QVERIFY(got.ok());
    QCOMPARE(got.value.results.size(), 1);
    QCOMPARE(got.value.results.first().count, 4);
    QCOMPARE(got.value.results.first().ancestorIds.last(), qint64(752416));

    const QString filtered = m_transport->received.at(0).url.toString();
    QVERIFY(filtered.contains(QStringLiteral("user_login=someone")));
    QVERIFY(filtered.contains(QStringLiteral("taxon_id=47217")));
    QVERIFY(filtered.contains(QStringLiteral("place_id=6744")));
    QVERIFY(filtered.contains(QStringLiteral("quality_grade=research")));
    QVERIFY(!filtered.contains(QStringLiteral("verifiable")));

    const QString plain = m_transport->received.at(1).url.toString();
    QVERIFY(!plain.contains(QStringLiteral("taxon_id")));
    QVERIFY(!plain.contains(QStringLiteral("place_id")));
    QVERIFY(!plain.contains(QStringLiteral("quality_grade")));
    QVERIFY(!plain.contains(QStringLiteral("verifiable")));
    QVERIFY(plain.contains(QStringLiteral("page=2")));
}

void TestINatClient::observationCountReadsTotalResults()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 321, "page": 1, "per_page": 0, "results": []})"));
    });

    Outcome<int> got;
    bool done = false;
    m_inat->observationCount(570544, 6744, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value, 321);

    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(url.contains(QStringLiteral("/observations?")));
    QVERIFY(url.contains(QStringLiteral("taxon_id=570544")));
    QVERIFY(url.contains(QStringLiteral("place_id=6744")));
    QVERIFY(url.contains(QStringLiteral("verifiable=true")));
    QVERIFY(url.contains(QStringLiteral("per_page=0")));
}

void TestINatClient::fetchAllObservationsSendsTaxonOnlyWhenGiven()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(R"({"total_results": 0, "results": []})"));
    });

    int calls = 0;
    m_inat->fetchAllObservations(QStringLiteral("someone"), 0, 47217, 0,
                                 [&](auto) { ++calls; });
    QTRY_COMPARE(calls, 1);
    m_inat->fetchAllObservations(QStringLiteral("someone"), 0, 0, 0, [&](auto) { ++calls; });
    QTRY_COMPARE(calls, 2);

    QVERIFY(m_transport->received.at(0).url.toString().contains(
        QStringLiteral("taxon_id=47217")));
    QVERIFY(!m_transport->received.at(1).url.toString().contains(QStringLiteral("taxon_id")));
}

void TestINatClient::fetchObservationsSendsDateRangeWhenProvided()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 1, "results": [
                {"id": 1, "observed_on": "2025-01-10", "taxon": {"id": 900},
                 "geojson": {"type": "Point", "coordinates": [144.9631, -37.8136]},
                 "photos": [{"id": 1, "url": "https://x/1/square.jpg"}]}
            ]})"));
    });

    Outcome<ObservationPage> got;
    bool done = false;
    m_inat->fetchObservations(QStringLiteral("someone"), {900}, 0, QDate(2025, 1, 9),
                              QDate(2025, 1, 11), 1, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(got.ok());
    QCOMPARE(got.value.totalResults, 1);
    QCOMPARE(got.value.results.size(), 1);
    QCOMPARE(got.value.results.first().latitude.value(), -37.8136);

    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(url.contains(QStringLiteral("d1=2025-01-09")));
    QVERIFY(url.contains(QStringLiteral("d2=2025-01-11")));
}

void TestINatClient::fetchObservationsOmitsDateParamsWhenInvalid()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(R"({"total_results": 0, "results": []})"));
    });

    bool done = false;
    m_inat->fetchObservations(QStringLiteral("someone"), {900}, 0, QDate(), QDate(), 1,
                              [&](auto) { done = true; });
    QTRY_VERIFY(done);

    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(!url.contains(QStringLiteral("d1=")));
    QVERIFY(!url.contains(QStringLiteral("d2=")));
}

void TestINatClient::fetchObservationsWithNoTaxaSearchesEveryTaxon()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(R"({"total_results": 0, "results": []})"));
    });

    bool done = false;
    m_inat->fetchObservations(QStringLiteral("someone"), {}, 0, QDate(2025, 1, 9), QDate(2025, 1, 11), 1,
                              [&](auto) { done = true; });
    QTRY_VERIFY(done);

    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(!url.contains(QStringLiteral("taxon_id")));
    QVERIFY(url.contains(QStringLiteral("user_login=someone")));
}

void TestINatClient::fetchObservationsCanIncludeThoseWithoutPhotos()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 1, "results": [{"id": 901, "observed_on": "2025-10-18",
                "taxon": {"id": 12936, "name": "Petroica phoenicea", "ancestor_ids": [48460, 1, 7000, 12936]}}]})"));
    });

    std::optional<Outcome<ObservationPage>> got;
    m_inat->fetchObservations(QStringLiteral("someone"), {}, 0, QDate(2025, 10, 17), QDate(2025, 10, 19), 1,
                              [&](Outcome<ObservationPage> out) { got = out; }, false);
    QTRY_VERIFY(got.has_value());
    QVERIFY(!m_transport->received.at(0).url.query().contains(QStringLiteral("photos=")));
    QCOMPARE(got->value.results.size(), 1);
    // The taxon's ancestors, without the taxon itself.
    QCOMPARE(got->value.results.at(0).taxonAncestorIds, (QList<qint64>{48460, 1, 7000}));

    bool done = false;
    m_inat->fetchObservations(QStringLiteral("someone"), {}, 0, QDate(), QDate(), 1, [&](auto) { done = true; });
    QTRY_VERIFY(done);
    QVERIFY(m_transport->received.at(1).url.query().contains(QStringLiteral("photos=true")));
}

void TestINatClient::fetchObservationsByIdGivesThoseStillThere()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 1, "results": [{"id": 901, "observed_on": "2025-10-18",
                "photos": [{"id": 5001, "url": "https://x/square.jpg"}]}]})"));
    });
    std::optional<Outcome<ObservationPage>> got;
    m_inat->fetchObservationsById({901, 902}, [&](Outcome<ObservationPage> out) { got = out; });
    QTRY_VERIFY(got.has_value());
    QCOMPARE(m_transport->received.at(0).url.toString(),
             QStringLiteral("https://api.test/v1/observations?id=901,902&per_page=200"));
    QCOMPARE(got->value.results.size(), 1);   // 902 was deleted
    QCOMPARE(got->value.results.at(0).photos.size(), 1);
    QCOMPARE(got->value.results.at(0).photos.at(0).id, 5001);
}

void TestINatClient::httpErrorBecomesOutcomeError()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(500); });

    Outcome<QList<taxonomy::Taxon>> got;
    bool done = false;
    m_inat->searchTaxa(QStringLiteral("x"), QString(), [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(!got.ok());
    QVERIFY(got.error.contains(QStringLiteral("500")));
}

void TestINatClient::invalidJsonBecomesOutcomeError()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral("<html>not json</html>"));
    });

    Outcome<SpeciesCountsPage> got;
    bool done = false;
    m_inat->speciesCounts(1, 0, 1, 10, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY(!got.ok());
}

// The photo goes as a multipart "image" part; the location and date as form
// fields (in the query string iNaturalist ignores them); the token as Bearer.
void TestINatClient::scoreImagePostsThePhotoWithWhereAndWhen()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"total_results": 2, "results": [
            {"combined_score": 93.5, "vision_score": 88.1, "frequency_score": 1,
             "taxon": {"id": 12936, "rank": "species", "name": "Petroica phoenicea",
                       "preferred_common_name": "Flame Robin"}},
            {"combined_score": 4.2, "vision_score": 9.0, "frequency_score": 0,
             "taxon": {"id": 12937, "rank": "species", "name": "Petroica boodang",
                       "preferred_common_name": "Scarlet Robin"}}],
            "common_ancestor": {"score": 97.7,
             "taxon": {"id": 12933, "rank": "genus", "name": "Petroica"}}})");
    });
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));

    Outcome<VisionResult> got;
    bool done = false;
    m_inat->scoreImage("JPEGDATA", QPointF(144.9796, -37.8304), QDate(2025, 10, 18),
                       [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);

    QVERIFY2(got.ok(), qPrintable(got.error));
    QCOMPARE(got.value.suggestions.size(), 2);
    QCOMPARE(got.value.suggestions.at(0).taxon.name, QStringLiteral("Petroica phoenicea"));
    QCOMPARE(got.value.suggestions.at(0).taxon.commonName, QStringLiteral("Flame Robin"));
    QCOMPARE(got.value.suggestions.at(0).score, 93.5);
    QVERIFY(got.value.suggestions.at(0).seenNearby);
    QVERIFY(!got.value.suggestions.at(1).seenNearby);
    QVERIFY(got.value.commonAncestor);
    QCOMPARE(got.value.commonAncestor->taxon.rank, QStringLiteral("genus"));
    QCOMPARE(got.value.commonAncestor->score, 97.7);

    const Transport::Request &req = m_transport->received.at(0);
    QCOMPARE(req.verb, QByteArray("POST"));
    QCOMPARE(req.url.toString(), QStringLiteral("https://api.test/v1/computervision/score_image"));
    QVERIFY(req.url.query().isEmpty());
    QCOMPARE(req.bearerToken, QStringLiteral("eyJ.token"));
    QVERIFY(req.contentType.startsWith("multipart/form-data; boundary="));
    const QByteArray boundary = req.contentType.mid(req.contentType.indexOf('=') + 1);
    QVERIFY(req.body.contains("--" + boundary + "\r\nContent-Disposition: form-data; name=\"lat\"\r\n\r\n-37.830400\r\n"));
    QVERIFY(req.body.contains("name=\"lng\"\r\n\r\n144.979600\r\n"));
    QVERIFY(req.body.contains("name=\"observed_on\"\r\n\r\n2025-10-18\r\n"));
    QVERIFY(req.body.contains("name=\"image\"; filename=\"photo.jpg\"\r\nContent-Type: image/jpeg\r\n\r\nJPEGDATA\r\n"));
    QVERIFY(req.body.endsWith("--" + boundary + "--\r\n"));
}

void TestINatClient::scoreImageReportsAnExpiredToken()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(401); });
    Outcome<VisionResult> got;
    bool done = false;
    m_inat->scoreImage("JPEG", std::nullopt, std::nullopt, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY(!got.ok());
    QCOMPARE(got.status, 401);
    QVERIFY(!m_transport->received.at(0).body.contains("name=\"lat\""));
}

void TestINatClient::fetchMeGivesTheLogin()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"total_results": 1, "results": [{"id": 77, "login": "someone"}]})");
    });
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));
    Outcome<QString> got;
    bool done = false;
    m_inat->fetchMe([&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(got.ok(), qPrintable(got.error));
    QCOMPARE(got.value, QStringLiteral("someone"));
    QCOMPARE(m_transport->received.at(0).url.toString(), QStringLiteral("https://api.test/v1/users/me"));
    QCOMPARE(m_transport->received.at(0).bearerToken, QStringLiteral("eyJ.token"));
}

void TestINatClient::createObservationPostsItsFields()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"id": 901, "uuid": "a-b-c"})");
    });
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));

    NewObservation obs;
    obs.uuid = QStringLiteral("a-b-c");
    obs.taxonId = 12936;
    obs.speciesGuess = QStringLiteral("Flame Robin");
    obs.date = QDate(2025, 10, 18);
    obs.time = QTime(8, 15, 30);
    obs.latitude = -37.8304;
    obs.longitude = 144.9796;
    obs.geoprivacy = QStringLiteral("obscured");
    obs.description = QStringLiteral("On the fence");
    obs.fromVision = true;
    Outcome<qint64> got;
    bool done = false;
    m_inat->createObservation(obs, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(got.ok(), qPrintable(got.error));
    QCOMPARE(got.value, 901);

    const Transport::Request &req = m_transport->received.at(0);
    QCOMPARE(req.verb, QByteArray("POST"));
    QCOMPARE(req.url.toString(), QStringLiteral("https://api.test/v1/observations"));
    QCOMPARE(req.bearerToken, QStringLiteral("eyJ.token"));
    QCOMPARE(req.contentType, QByteArray("application/json"));
    const QJsonObject o = QJsonDocument::fromJson(req.body).object().value(QStringLiteral("observation")).toObject();
    QCOMPARE(o.value(QStringLiteral("uuid")).toString(), QStringLiteral("a-b-c"));
    QCOMPARE(o.value(QStringLiteral("taxon_id")).toInteger(), 12936);
    QCOMPARE(o.value(QStringLiteral("species_guess")).toString(), QStringLiteral("Flame Robin"));
    QCOMPARE(o.value(QStringLiteral("observed_on_string")).toString(), QStringLiteral("2025-10-18 08:15:30"));
    QCOMPARE(o.value(QStringLiteral("latitude")).toDouble(), -37.8304);
    QCOMPARE(o.value(QStringLiteral("longitude")).toDouble(), 144.9796);
    QCOMPARE(o.value(QStringLiteral("geoprivacy")).toString(), QStringLiteral("obscured"));
    QCOMPARE(o.value(QStringLiteral("description")).toString(), QStringLiteral("On the fence"));
    QVERIFY(o.value(QStringLiteral("owners_identification_from_vision")).toBool());
}

// A refusal comes back with iNaturalist's own reason, and a server error
// isn't sent again (it may have made the observation).
void TestINatClient::createObservationSaysWhatIsWrong()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::httpStatus(422, R"({"error": {"original": {"error": "Latitude is not a number"}}, "status": 422})");
    });
    NewObservation obs;
    obs.date = QDate(2025, 10, 18);
    Outcome<qint64> got;
    bool done = false;
    m_inat->createObservation(obs, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(got.status, 422);
    QVERIFY2(got.error.contains(QStringLiteral("Latitude is not a number")), qPrintable(got.error));
    QVERIFY(!QJsonDocument::fromJson(m_transport->received.at(0).body).object()
                 .value(QStringLiteral("observation")).toObject().contains(QStringLiteral("time")));

    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(502); });
    done = false;
    m_inat->createObservation(obs, [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(got.status, 502);
    QCOMPARE(m_transport->received.size(), 1);
}

void TestINatClient::findObservationByUuidTellsWhetherItExists()
{
    wire([](const Transport::Request &req, int) {
        if (req.url.query().contains(QStringLiteral("made")))
            return FakeTransport::ok(R"({"total_results": 1, "results": [{"id": 901}]})");
        return FakeTransport::ok(R"({"total_results": 0, "results": []})");
    });
    Outcome<std::optional<qint64>> got;
    bool done = false;
    m_inat->findObservationByUuid(QStringLiteral("made"), [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY(got.ok());
    QCOMPARE(got.value, std::optional<qint64>(901));
    done = false;
    m_inat->findObservationByUuid(QStringLiteral("never"), [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY(got.ok());
    QVERIFY(!got.value);
    QCOMPARE(m_transport->received.at(1).url.toString(), QStringLiteral("https://api.test/v1/observations?uuid=never"));
}

void TestINatClient::addObservationPhotoPostsTheFile()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"id": 5, "observation_id": 901, "photo": {"id": 4401, "url": "x"}})");
    });
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));
    Outcome<qint64> got;
    bool done = false;
    m_inat->addObservationPhoto(901, "JPEGDATA", QStringLiteral("Flame Robin 1.jpg"), [&](auto o) { got = o; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(got.ok(), qPrintable(got.error));
    QCOMPARE(got.value, 4401);
    const Transport::Request &req = m_transport->received.at(0);
    QCOMPARE(req.url.toString(), QStringLiteral("https://api.test/v1/observation_photos"));
    QCOMPARE(req.bearerToken, QStringLiteral("eyJ.token"));
    QVERIFY(req.body.contains("name=\"observation_photo[observation_id]\"\r\n\r\n901\r\n"));
    QVERIFY(req.body.contains("name=\"file\"; filename=\"Flame Robin 1.jpg\"\r\nContent-Type: image/jpeg\r\n\r\nJPEGDATA\r\n"));
}

QTEST_GUILESS_MAIN(TestINatClient)
#include "tst_inatclient.moc"
