#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "inat/InatObservationFetcher.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QUrlQuery>

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;
using namespace pl::inat;

class TestInatObservationFetcher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void flagsAnObservationMatchingAnExistingLocalCapture();
    void doesNotFlagAnObservationOfADifferentTaxon();
    void paginatesUntilTotalResultsIsReached();
    void fetchErrorFailsTheRun();
    void placeIdIsSentWhenProvidedAndOmittedWhenNot();
    void flagsAnExactPhotoIdMatchEvenWithoutATaxonMatch();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<InatObservationFetcher> m_fetcher;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    void seedLocalCapture(qint64 taxonInatId, const QString &date, double lat, double lon);
    void seedDownloadedPhoto(qint64 observationId, qint64 photoId);
};

void TestInatObservationFetcher::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());

    Taxon t;
    t.inatId = 900;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Diuris sp900");
    QVERIFY(m_store->upsertTaxon(t) > 0);

    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) "
                             "VALUES (1, '/lib', 'lib', 0)"));
}

void TestInatObservationFetcher::cleanup()
{
    m_fetcher.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

void TestInatObservationFetcher::wire(
    std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_http = std::make_unique<HttpClient>(std::move(owned), m_store.get());
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_fetcher = std::make_unique<InatObservationFetcher>(*m_inat, m_db->connectionName());
}

void TestInatObservationFetcher::seedLocalCapture(qint64 taxonInatId, const QString &date,
                                                  double lat, double lon)
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery cap(db);
    cap.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name, captured_on, latitude, longitude) "
        "VALUES (1, 'x', ?, ?, ?)"));
    cap.addBindValue(date);
    cap.addBindValue(lat);
    cap.addBindValue(lon);
    QVERIFY(cap.exec());
    const int captureId = cap.lastInsertId().toInt();

    QSqlQuery m(db);
    m.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
        "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'manual', 1.0, 'confirmed', 'user')"));
    m.addBindValue(captureId);
    m.addBindValue(taxonInatId);
    QVERIFY(m.exec());
}

// A capture stamped with iNat provenance but never reviewed/matched to a
// taxon -- exercises the exact-id check's independence from the
// taxon+date+GPS heuristic, which requires a capture_match row to see
// anything at all.
void TestInatObservationFetcher::seedDownloadedPhoto(qint64 observationId, qint64 photoId)
{
    QSqlQuery cap(QSqlDatabase::database(m_db->connectionName(), false));
    cap.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name, inat_observation_id, inat_photo_id) "
        "VALUES (1, 'y', ?, ?)"));
    cap.addBindValue(observationId);
    cap.addBindValue(photoId);
    QVERIFY(cap.exec());
}

void TestInatObservationFetcher::flagsAnObservationMatchingAnExistingLocalCapture()
{
    seedLocalCapture(900, QStringLiteral("2025-01-10"), -37.8136, 144.9631);

    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"total_results":1,"results":[
            {"id":1,"observed_on":"2025-01-10","taxon":{"id":900},
             "geojson":{"type":"Point","coordinates":[144.9631,-37.8136]},
             "photos":[{"id":1,"url":"https://x/1/square.jpg"}]}
        ]})");
    });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900});
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const auto candidates = spy.at(0).at(2).value<QList<Candidate>>();
    QCOMPARE(candidates.size(), 1);
    QVERIFY(candidates.first().likelyDuplicate);
}

void TestInatObservationFetcher::doesNotFlagAnObservationOfADifferentTaxon()
{
    seedLocalCapture(900, QStringLiteral("2025-01-10"), -37.8136, 144.9631);

    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"total_results":1,"results":[
            {"id":2,"observed_on":"2025-01-10","taxon":{"id":901},
             "geojson":{"type":"Point","coordinates":[144.9631,-37.8136]},
             "photos":[{"id":2,"url":"https://x/2/square.jpg"}]}
        ]})");
    });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900, 901});
    QVERIFY(spy.wait(5000));

    const auto candidates = spy.at(0).at(2).value<QList<Candidate>>();
    QCOMPARE(candidates.size(), 1);
    QVERIFY(!candidates.first().likelyDuplicate);
}

void TestInatObservationFetcher::paginatesUntilTotalResultsIsReached()
{
    wire([](const Transport::Request &req, int call) {
        Q_UNUSED(req);
        const QString id = QString::number(call + 1);
        return FakeTransport::ok(QStringLiteral(R"({"total_results":2,"results":[
            {"id":%1,"observed_on":"2025-01-10","taxon":{"id":900},
             "photos":[{"id":%1,"url":"https://x/%1/square.jpg"}]}
        ]})").arg(id).toUtf8());
    });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900});
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const auto candidates = spy.at(0).at(2).value<QList<Candidate>>();
    QCOMPARE(candidates.size(), 2);   // both pages' results collected
}

void TestInatObservationFetcher::fetchErrorFailsTheRun()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(500); });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900});
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
}

void TestInatObservationFetcher::placeIdIsSentWhenProvidedAndOmittedWhenNot()
{
    QList<QUrl> capturedUrls;
    wire([&capturedUrls](const Transport::Request &req, int) {
        capturedUrls << req.url;
        return FakeTransport::ok(QByteArrayLiteral(R"({"total_results":0,"results":[]})"));
    });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900}, 6744);
    QVERIFY(spy.wait(5000));
    QCOMPARE(capturedUrls.size(), 1);
    QCOMPARE(QUrlQuery(capturedUrls.first()).queryItemValue(QStringLiteral("place_id")),
             QStringLiteral("6744"));

    capturedUrls.clear();
    QSignalSpy spy2(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900});   // no placeId -> defaults to 0 (worldwide)
    QVERIFY(spy2.wait(5000));
    QCOMPARE(capturedUrls.size(), 1);
    QVERIFY(!QUrlQuery(capturedUrls.first()).hasQueryItem(QStringLiteral("place_id")));
}

void TestInatObservationFetcher::flagsAnExactPhotoIdMatchEvenWithoutATaxonMatch()
{
    seedDownloadedPhoto(5, 55);   // photo 55 already downloaded, never matched to a taxon

    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"total_results":1,"results":[
            {"id":5,"observed_on":"2025-01-10","taxon":{"id":900},
             "photos":[{"id":55,"url":"https://x/55/square.jpg"},
                       {"id":56,"url":"https://x/56/square.jpg"}]}
        ]})");
    });

    QSignalSpy spy(m_fetcher.get(), &InatObservationFetcher::finished);
    m_fetcher->start(QStringLiteral("someone"), {900});
    QVERIFY(spy.wait(5000));

    const auto candidates = spy.at(0).at(2).value<QList<Candidate>>();
    QCOMPARE(candidates.size(), 1);
    // The heuristic alone would miss this -- no capture_match row exists to
    // join through -- but the exact photo-id check still catches photo 55.
    QVERIFY(!candidates.first().likelyDuplicate);
    QCOMPARE(candidates.first().alreadyDownloadedPhotoIds, QSet<qint64>{55});
}

QTEST_MAIN(TestInatObservationFetcher)
#include "tst_inatobservationfetcher.moc"
