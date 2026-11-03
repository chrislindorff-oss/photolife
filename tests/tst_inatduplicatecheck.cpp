#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "inat/InatDuplicateCheck.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::inat;

namespace {

constexpr qint64 kRobin = 12936;

QJsonObject observation(qint64 id, qint64 taxon, const QString &at, double lat)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("observed_on"), at.left(10)},
        {QStringLiteral("time_observed_at"), at},
        {QStringLiteral("taxon"), QJsonObject{{QStringLiteral("id"), taxon}, {QStringLiteral("name"), QStringLiteral("X")}}},
        {QStringLiteral("geojson"), QJsonObject{{QStringLiteral("coordinates"), QJsonArray{144.98, lat}}}},
    };
}

QByteArray page(int total, const QJsonArray &results)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("total_results"), total}, {QStringLiteral("results"), results}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

class TestInatDuplicateCheck : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void findsItInTheLibraryAndOnINaturalist();
    void followsPagesAndJoinsDays();
    void withoutAUsernameOnlyTheLibraryIsAsked();

private:
    std::unique_ptr<Database> m_db;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<InatDuplicateCheck> m_check;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    static InatDuplicateCheck::Group robinAt(int key, const QDateTime &when);
};

void TestInatDuplicateCheck::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(m_db->connectionName());
    taxonomy::Taxon robin;
    robin.inatId = kRobin;
    robin.rank = QStringLiteral("species");
    robin.name = QStringLiteral("Petroica phoenicea");
    robin.ancestry = QStringLiteral("48460/1/8000");
    QVERIFY(store.upsertTaxon(robin) > 0);
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    const QStringList setup{
        QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)"),
        // A photo of the user's already on observation 777, ten minutes after.
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on, latitude, longitude, "
                       "inat_observation_id, inat_photo_id) VALUES (1, 1, 'IMG_1', '2025-10-18T08:10:00', -37.83, 144.98, 777, 4401)"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (1, '/lib/IMG_1.JPG', 'jpeg', 'jpg', 'h1')"),
        QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                       "SELECT 1, id, 'name', 1.0, 'confirmed', 'user' FROM taxon WHERE inat_id = 12936"),
    };
    for (const QString &sql : setup)
        QVERIFY2(q.exec(sql), qPrintable(sql));
}

void TestInatDuplicateCheck::cleanup()
{
    m_check.reset();
    m_inat.reset();
    m_http.reset();
    m_db.reset();
}

void TestInatDuplicateCheck::wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    m_check.reset();
    m_inat.reset();
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_check = std::make_unique<InatDuplicateCheck>(*m_inat, m_db->connectionName());
}

InatDuplicateCheck::Group TestInatDuplicateCheck::robinAt(int key, const QDateTime &when)
{
    InatDuplicateCheck::Group g;
    g.key = key;
    g.observation.taxon.inatId = kRobin;
    UploadPhoto p;
    p.captureId = 100 + key;
    p.taken = when;
    p.hasTime = true;
    p.latitude = -37.83;
    p.longitude = 144.98;
    g.observation.photos << p;
    return g;
}

void TestInatDuplicateCheck::findsItInTheLibraryAndOnINaturalist()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(page(2, {observation(901, kRobin, QStringLiteral("2025-10-18T08:20:00+11:00"), -37.8301),
                                          observation(777, kRobin, QStringLiteral("2025-10-18T08:10:00+11:00"), -37.83)}));
    });
    QSignalSpy finished(m_check.get(), &InatDuplicateCheck::finished);
    m_check->start(QStringLiteral("someone"),
                   {robinAt(5, QDateTime(QDate(2025, 10, 18), QTime(8, 0))),
                    robinAt(6, QDateTime(QDate(2025, 10, 18), QTime(23, 30)))});   // 15 hours later: no
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(finished.at(0).at(0).toBool());
    const auto hits = finished.at(0).at(3).value<QHash<int, QList<DuplicateHit>>>();
    QCOMPARE(hits.keys(), (QList<int>{5}));
    QCOMPARE(hits.value(5).size(), 2);   // 777 once, as iNaturalist has it
    QCOMPARE(hits.value(5).at(0).existing.id, 777);
    QVERIFY(!hits.value(5).at(0).existing.fromLibrary);
    QCOMPARE(hits.value(5).at(1).existing.id, 901);

    const QUrlQuery query(m_transport->received.at(0).url);
    QCOMPARE(query.queryItemValue(QStringLiteral("user_login")), QStringLiteral("someone"));
    QCOMPARE(query.queryItemValue(QStringLiteral("d1")), QStringLiteral("2025-10-17"));
    QCOMPARE(query.queryItemValue(QStringLiteral("d2")), QStringLiteral("2025-10-19"));
    QVERIFY(!query.hasQueryItem(QStringLiteral("photos")));   // those without photos count too
}

void TestInatDuplicateCheck::followsPagesAndJoinsDays()
{
    wire([](const Transport::Request &req, int) {
        const QUrlQuery query(req.url);
        if (query.queryItemValue(QStringLiteral("page")) == QStringLiteral("2"))
            return FakeTransport::ok(page(201, {observation(999, kRobin, QStringLiteral("2025-10-20T08:05:00+11:00"), -37.83)}));
        QJsonArray far;
        for (int i = 0; i < InatDuplicateCheck::kPerPage; ++i)
            far.append(observation(2000 + i, kRobin, QStringLiteral("2025-10-20T08:05:00+11:00"), -30.0));
        return FakeTransport::ok(page(201, far));
    });
    QSignalSpy finished(m_check.get(), &InatDuplicateCheck::finished);
    m_check->start(QStringLiteral("someone"), {robinAt(1, QDateTime(QDate(2025, 10, 18), QTime(8, 0))),
                                               robinAt(2, QDateTime(QDate(2025, 10, 20), QTime(8, 0)))});
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(m_transport->received.size(), 2);   // one range, two pages
    QCOMPARE(QUrlQuery(m_transport->received.at(0).url).queryItemValue(QStringLiteral("d2")), QStringLiteral("2025-10-21"));
    const auto hits = finished.at(0).at(3).value<QHash<int, QList<DuplicateHit>>>();
    QCOMPARE(hits.value(2).size(), 1);
    QCOMPARE(hits.value(2).at(0).existing.id, 999);
    QCOMPARE(hits.value(1).at(0).existing.id, 777);   // from the library
}

void TestInatDuplicateCheck::withoutAUsernameOnlyTheLibraryIsAsked()
{
    wire([](const Transport::Request &, int) { return FakeTransport::ok(page(0, {})); });
    QSignalSpy finished(m_check.get(), &InatDuplicateCheck::finished);
    m_check->start(QString(), {robinAt(1, QDateTime(QDate(2025, 10, 18), QTime(8, 0)))});
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.at(0).at(0).toBool());
    QVERIFY(m_transport->received.isEmpty());
    const auto hits = finished.at(0).at(3).value<QHash<int, QList<DuplicateHit>>>();
    QCOMPARE(hits.value(1).size(), 1);
    QVERIFY(hits.value(1).at(0).existing.fromLibrary);
    QCOMPARE(hits.value(1).at(0).existing.libraryPhoto, QStringLiteral("IMG_1.JPG"));
}

QTEST_GUILESS_MAIN(TestInatDuplicateCheck)
#include "tst_inatduplicatecheck.moc"
