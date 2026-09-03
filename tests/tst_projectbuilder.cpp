#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

namespace {

Transport::Reply route(const Transport::Request &req)
{
    const QString path = req.url.path();
    const QString url = req.url.toString();

    if (path.endsWith(QLatin1String("/places/autocomplete"))) {
        return FakeTransport::ok(R"({"results":[
            {"id":6744,"name":"Victoria","display_name":"Victoria, AU","admin_level":10}
        ]})");
    }
    if (path.endsWith(QLatin1String("/taxa")) && url.contains(QLatin1String("q="))) {
        return FakeTransport::ok(R"({"results":[
            {"id":47217,"rank":"family","rank_level":30,"name":"Orchidaceae",
             "is_active":true,"ancestry":"48460/47126"}
        ]})");
    }
    if (path.endsWith(QLatin1String("/taxa/47217"))) {
        return FakeTransport::ok(R"({"results":[{
            "id":47217,"rank":"family","rank_level":30,"name":"Orchidaceae",
            "ancestors":[
                {"id":48460,"rank":"kingdom","rank_level":70,"name":"Plantae"},
                {"id":47126,"rank":"phylum","rank_level":60,"name":"Tracheophyta"}
            ],
            "children":[{"id":800,"rank":"genus","name":"Diuris"}]
        }]})");
    }
    if (path.contains(QLatin1String("/observations/species_counts"))) {
        return FakeTransport::ok(R"({
            "total_results":2,"page":1,"per_page":200,
            "results":[
                {"count":100,"taxon":{"id":900,"rank":"species","rank_level":10,
                 "name":"Diuris pardina","ancestry":"48460/47126/47217/800"}},
                {"count":50,"taxon":{"id":901,"rank":"species","rank_level":10,
                 "name":"Caladenia carnea","ancestry":"48460/47126/47217/801"}}
            ]
        })");
    }
    if (path.contains(QLatin1String("/taxa/"))) {   // batch fill, e.g. /v1/taxa/800,801
        return FakeTransport::ok(R"({"results":[
            {"id":800,"rank":"genus","rank_level":20,"name":"Diuris","ancestry":"48460/47126/47217"},
            {"id":801,"rank":"genus","rank_level":20,"name":"Caladenia","ancestry":"48460/47126/47217"}
        ]})");
    }
    return FakeTransport::httpStatus(404);
}

} // namespace

class TestProjectBuilder : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void buildsConnectedTree();
    void speciesListErrorFailsTheBuild();
    void cancellationStopsTheBuild();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<pl::net::INatClient> m_inat;
    std::unique_ptr<ProjectBuilder> m_builder;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    ProjectBuilder::Request request() const;
};

void TestProjectBuilder::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
}

void TestProjectBuilder::cleanup()
{
    m_builder.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

void TestProjectBuilder::wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), m_store.get());
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<pl::net::INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_builder = std::make_unique<ProjectBuilder>(*m_inat, *m_store);
}

ProjectBuilder::Request TestProjectBuilder::request() const
{
    ProjectBuilder::Request r;
    r.projectName = QStringLiteral("Orchids of Victoria");
    r.taxonQuery = QStringLiteral("Orchidaceae");
    r.rank = QStringLiteral("family");
    r.placeQuery = QStringLiteral("Victoria, AU");
    return r;
}

void TestProjectBuilder::buildsConnectedTree()
{
    wire([](const Transport::Request &req, int) { return route(req); });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);           // ok
    const int projectId = spy.at(0).at(2).toInt();
    QVERIFY(projectId > 0);

    // family + 2 spine ancestors + 2 species + 2 genera
    QCOMPARE(m_store->taxonCount(), 7);
    QCOMPARE(m_store->projectTaxonInatIds(projectId).size(), 7);

    const auto tree = m_store->projectTree(projectId);
    QCOMPARE(tree.size(), 7);
    QCOMPARE(tree.first().rank, QStringLiteral("kingdom"));

    bool sawInRegionSpecies = false;
    for (const auto &n : tree) {
        if (n.name == QStringLiteral("Diuris pardina")) {
            sawInRegionSpecies = n.inRegion && n.isLeafRank;
            QCOMPARE(n.parentInatId.value_or(-1), qint64(800));
        }
    }
    QVERIFY(sawInRegionSpecies);

    QVERIFY(m_store->placeByInatId(6744).has_value());

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT refreshed_at FROM project WHERE id = ") + QString::number(projectId));
    QVERIFY(q.next());
    QVERIFY(!q.value(0).isNull());
}

void TestProjectBuilder::speciesListErrorFailsTheBuild()
{
    wire([](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("species_counts")))
            return FakeTransport::httpStatus(404);
        return route(req);
    });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    QVERIFY(!m_builder->isRunning());
}

void TestProjectBuilder::cancellationStopsTheBuild()
{
    wire([this](const Transport::Request &req, int call) -> Transport::Reply {
        if (call == 1)
            m_builder->cancel();   // cancel after the place lookup
        return route(req);
    });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("cancelled"));
}

QTEST_GUILESS_MAIN(TestProjectBuilder)
#include "tst_projectbuilder.moc"
