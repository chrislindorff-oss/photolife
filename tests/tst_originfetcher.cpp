#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/OriginFetcher.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

class TestOriginFetcher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void recordsIntroducedTaxaAcrossPagesAndStampsTheTree();
    void failedFetchLeavesTheTreeToRetry();
    void skipsTreesWithoutARegion();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<OriginFetcher> m_fetcher;
    FakeTransport *m_transport = nullptr;
    int m_projectId = -1;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    int scalar(const QString &sql);
};

void TestOriginFetcher::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());

    for (qint64 id : {qint64(47125), qint64(56024), qint64(56025), qint64(900)}) {
        Taxon t;
        t.inatId = id;
        t.rank = id == 47125 ? QStringLiteral("subclass") : QStringLiteral("species");
        t.name = QStringLiteral("Taxon %1").arg(id);
        m_store->upsertTaxon(t);
    }
    Place vic;
    vic.inatId = 7830;
    vic.name = QStringLiteral("Victoria");
    QVERIFY(m_store->upsertPlace(vic));
    m_projectId = m_store->ensureProject(QStringLiteral("Plants of Victoria"), 47125, 7830,
                                         QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
}

void TestOriginFetcher::cleanup()
{
    m_fetcher.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

void TestOriginFetcher::wire(
    std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_fetcher = std::make_unique<OriginFetcher>(*m_inat, *m_store);
}

int TestOriginFetcher::scalar(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

void TestOriginFetcher::recordsIntroducedTaxaAcrossPagesAndStampsTheTree()
{
    // Two pages: 56024 and 56025, then 12345 (not in the cache: skipped).
    wire([](const Transport::Request &r, int index) {
        Q_UNUSED(r);
        if (index == 0)
            return FakeTransport::ok(R"({"total_results": 3, "page": 1, "per_page": 2,
                "results": [{"count": 5, "taxon": {"id": 56024, "rank": "species", "name": "A"}},
                            {"count": 2, "taxon": {"id": 56025, "rank": "species", "name": "B"}}]})");
        return FakeTransport::ok(R"({"total_results": 3, "page": 2, "per_page": 2,
            "results": [{"count": 1, "taxon": {"id": 12345, "rank": "species", "name": "C"}}]})");
    });
    QCOMPARE(m_store->projectsMissingOrigins(), QList<int>{m_projectId});

    QSignalSpy spy(m_fetcher.get(), &OriginFetcher::projectFinished);
    m_fetcher->enqueue({m_projectId});
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toInt(), m_projectId);
    QVERIFY(spy.at(0).at(1).toBool());
    QCOMPARE(spy.at(0).at(2).toInt(), 2);
    QCOMPARE(m_transport->received.size(), 2);
    const QString url = m_transport->received.at(0).url.toString();
    QVERIFY(url.contains(QStringLiteral("introduced=true")));
    QVERIFY(url.contains(QStringLiteral("taxon_id=47125")));
    QVERIFY(url.contains(QStringLiteral("place_id=7830")));

    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_origin WHERE origin = 'introduced' "
                 "AND source = 'iNaturalist' AND place_inat_id = 7830")),
             2);
    QVERIFY(m_store->projectsMissingOrigins().isEmpty());
    QVERIFY(!m_fetcher->isRunning());
}

void TestOriginFetcher::failedFetchLeavesTheTreeToRetry()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(404); });

    QSignalSpy spy(m_fetcher.get(), &OriginFetcher::projectFinished);
    m_fetcher->enqueue({m_projectId});
    QVERIFY(spy.wait(5000));

    QVERIFY(!spy.at(0).at(1).toBool());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM taxon_origin")), 0);
    QCOMPARE(m_store->projectsMissingOrigins(), QList<int>{m_projectId});
}

void TestOriginFetcher::skipsTreesWithoutARegion()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(500); });
    const int noRegion = m_store->ensureProject(QStringLiteral("Grasses"), 47125, std::nullopt,
                                                QStringLiteral("inat"));
    QVERIFY(!m_store->projectsMissingOrigins().contains(noRegion));

    QSignalSpy spy(m_fetcher.get(), &OriginFetcher::projectFinished);
    m_fetcher->enqueue({noRegion});
    QVERIFY(!spy.wait(200));
    QCOMPARE(m_transport->received.size(), 0);
    QVERIFY(!m_fetcher->isRunning());
}

QTEST_GUILESS_MAIN(TestOriginFetcher)
#include "tst_originfetcher.moc"
