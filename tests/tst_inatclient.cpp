#include <QtTest>

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
    void observationCountReadsTotalResults();
    void httpErrorBecomesOutcomeError();
    void invalidJsonBecomesOutcomeError();

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

QTEST_GUILESS_MAIN(TestINatClient)
#include "tst_inatclient.moc"
