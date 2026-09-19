#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrlQuery>

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
    void usesConfirmedMatchesWithoutSearching();
    void largeSpeciesListChecksInWithTheUser();
    void refreshSkipsPrunedTaxaAndTheirNewChildren();
    void resumeAfterInterruptionCompletesWithoutDroppingAPage();
    void mismatchedCheckpointParamsAreDiscarded();
    void checkpointClearedOnSuccessfulFinish();

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

void TestProjectBuilder::usesConfirmedMatchesWithoutSearching()
{
    wire([](const Transport::Request &req, int) -> Transport::Reply {
        const QString path = req.url.path();
        if (path.endsWith(QLatin1String("/places/autocomplete"))
            || (path.endsWith(QLatin1String("/taxa")) && req.url.toString().contains(QLatin1String("q="))))
            return FakeTransport::httpStatus(500);   // must not be called: a match was already confirmed
        return route(req);
    });

    Place place;
    place.inatId = 6744;
    place.name = QStringLiteral("Victoria");
    place.displayName = QStringLiteral("Victoria, AU");
    place.adminLevel = 10;

    Taxon taxon;
    taxon.inatId = 47217;
    taxon.rank = QStringLiteral("family");
    taxon.rankLevel = 30;
    taxon.name = QStringLiteral("Orchidaceae");
    taxon.isActive = true;
    taxon.ancestry = QStringLiteral("48460/47126");

    ProjectBuilder::Request r = request();
    r.confirmedPlace = place;
    r.confirmedTaxon = taxon;

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(r);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);   // ok
    const int projectId = spy.at(0).at(2).toInt();
    QVERIFY(projectId > 0);
    QCOMPARE(m_store->taxonCount(), 7);
    QVERIFY(m_store->placeByInatId(6744).has_value());
}

void TestProjectBuilder::largeSpeciesListChecksInWithTheUser()
{
    constexpr int kTotal = 20000;

    wire([kTotal](const Transport::Request &req, int) -> Transport::Reply {
        const QString path = req.url.path();
        if (path.contains(QLatin1String("/observations/species_counts"))) {
            const QUrlQuery q(req.url);
            const int page = q.queryItemValue(QStringLiteral("page")).toInt();
            const int perPage = q.queryItemValue(QStringLiteral("per_page")).toInt();
            const int start = (page - 1) * perPage;
            QStringList rows;
            for (int i = start; i < qMin(start + perPage, kTotal); ++i) {
                const qint64 id = 100000 + i;
                rows << QStringLiteral(
                    "{\"count\":1,\"taxon\":{\"id\":%1,\"rank\":\"species\","
                    "\"rank_level\":10,\"name\":\"Testus sp%1\","
                    "\"ancestry\":\"48460/47126/47217/800\"}}").arg(id);
            }
            return FakeTransport::ok(
                QStringLiteral("{\"total_results\":%1,\"page\":%2,\"per_page\":%3,\"results\":[%4]}")
                    .arg(kTotal).arg(page).arg(perPage).arg(rows.join(QLatin1Char(',')))
                    .toUtf8());
        }
        return route(req);
    });

    ProjectBuilder::Request r = request();
    r.perPage = 5000;   // 2 pages to reach the 10,000 checkpoint

    QSignalSpy confirmSpy(m_builder.get(), &ProjectBuilder::confirmMoreSpecies);
    QSignalSpy finishSpy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(r);

    // First check-in at 10,000, with the estimated total, and the build paused.
    QVERIFY(confirmSpy.wait(5000));
    QCOMPARE(confirmSpy.at(0).at(0).toInt(), 10000);
    QCOMPARE(confirmSpy.at(0).at(1).toInt(), kTotal);
    QVERIFY(m_builder->isRunning());
    QVERIFY(finishSpy.isEmpty());

    // Keep going -> next check-in 5,000 species later.
    m_builder->continueFetching();
    QVERIFY(confirmSpy.wait(5000));
    QCOMPARE(confirmSpy.at(1).at(0).toInt(), 15000);

    // Stop here -> a normal, successful finish with the partial tree kept.
    m_builder->stopFetching();
    QVERIFY(finishSpy.wait(5000));
    QCOMPARE(finishSpy.at(0).at(0).toBool(), true);
    const int projectId = finishSpy.at(0).at(2).toInt();
    QVERIFY(projectId > 0);
    QVERIFY(m_store->projectTaxonInatIds(projectId).size() >= 15000);
}

void TestProjectBuilder::refreshSkipsPrunedTaxaAndTheirNewChildren()
{
    // A refresh must never resurrect a deliberately pruned taxon, and pruning
    // a whole genus must keep excluding species iNaturalist adds under it
    // *after* the prune too, not just the ones that existed at prune time.
    bool refreshing = false;
    wire([&refreshing](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/observations/species_counts"))) {
            const QString extra = refreshing
                ? QStringLiteral(
                      ",{\"count\":1,\"taxon\":{\"id\":902,\"rank\":\"species\","
                      "\"rank_level\":10,\"name\":\"Caladenia nova\","
                      "\"ancestry\":\"48460/47126/47217/801\"}}")
                : QString();
            return FakeTransport::ok(
                QStringLiteral(
                    "{\"total_results\":%1,\"page\":1,\"per_page\":200,\"results\":["
                    "{\"count\":100,\"taxon\":{\"id\":900,\"rank\":\"species\",\"rank_level\":10,"
                    "\"name\":\"Diuris pardina\",\"ancestry\":\"48460/47126/47217/800\"}},"
                    "{\"count\":50,\"taxon\":{\"id\":901,\"rank\":\"species\",\"rank_level\":10,"
                    "\"name\":\"Caladenia carnea\",\"ancestry\":\"48460/47126/47217/801\"}}"
                    "%2]}")
                    .arg(refreshing ? 3 : 2)
                    .arg(extra)
                    .toUtf8());
        }
        return route(req);
    });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const int projectId = spy.at(0).at(2).toInt();
    QVERIFY(projectId > 0);
    QCOMPARE(m_store->projectTaxonInatIds(projectId).size(), 7);

    // Prune one species directly, and a whole genus (with its one species).
    QVERIFY(m_store->pruneTaxonFromProject(projectId, 900) >= 0);   // Diuris pardina
    QVERIFY(m_store->pruneTaxonFromProject(projectId, 801) >= 0);   // Caladenia genus + species

    const auto afterPrune = m_store->projectTaxonInatIds(projectId);
    QVERIFY(!afterPrune.contains(900));
    QVERIFY(!afterPrune.contains(801));
    QVERIFY(!afterPrune.contains(901));

    refreshing = true;
    spy.clear();
    m_builder->start(request());   // same project name -> same project: a "refresh"
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(2).toInt(), projectId);

    const auto afterRefresh = m_store->projectTaxonInatIds(projectId);
    QVERIFY(!afterRefresh.contains(900));   // directly pruned -- still gone
    QVERIFY(!afterRefresh.contains(801));   // pruned genus -- still gone
    QVERIFY(!afterRefresh.contains(901));   // its species -- still gone
    QVERIFY(!afterRefresh.contains(902));   // new species under the pruned genus -- also excluded
    QVERIFY(afterRefresh.contains(800));    // untouched sibling genus/species survive
    QVERIFY(afterRefresh.contains(47217));
}

namespace {

// Serves a synthetic species_counts list of `total` species (all in one
// genus, 800, under the standard Orchidaceae/Plantae ancestry from route()),
// paginated by the request's own page/per_page query params -- unlike
// route()'s canned species_counts response, this one actually varies by page,
// which the resume tests below need.
Transport::Reply pagedSpecies(const Transport::Request &req, int total)
{
    const QUrlQuery q(req.url);
    const int page = q.queryItemValue(QStringLiteral("page")).toInt();
    const int perPage = q.queryItemValue(QStringLiteral("per_page")).toInt();
    const int start = (page - 1) * perPage;
    QStringList rows;
    for (int i = start; i < qMin(start + perPage, total); ++i) {
        const qint64 id = 100000 + i;
        rows << QStringLiteral(
            "{\"count\":1,\"taxon\":{\"id\":%1,\"rank\":\"species\","
            "\"rank_level\":10,\"name\":\"Testus sp%1\","
            "\"ancestry\":\"48460/47126/47217/800\"}}").arg(id);
    }
    return FakeTransport::ok(
        QStringLiteral("{\"total_results\":%1,\"page\":%2,\"per_page\":%3,\"results\":[%4]}")
            .arg(total).arg(page).arg(perPage).arg(rows.join(QLatin1Char(',')))
            .toUtf8());
}

// route()'s generic "/taxa/<ids>" ancestor-batch fallback always returns a
// fixed two-genus (800 and 801) response regardless of which ids were
// actually requested -- fine for fixtures that need both, but the resume
// tests below need only genus 800, so they route that one request here
// instead of falling through to route().
Transport::Reply singleGenusBatch()
{
    return FakeTransport::ok(R"({"results":[
        {"id":800,"rank":"genus","rank_level":20,"name":"Diuris","ancestry":"48460/47126/47217"}
    ]})");
}

} // namespace

void TestProjectBuilder::resumeAfterInterruptionCompletesWithoutDroppingAPage()
{
    constexpr int kTotal = 40;
    constexpr int kPerPage = 10;   // 4 pages

    int speciesCalls = 0;
    wire([&](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/observations/species_counts"))) {
            ++speciesCalls;
            // Interrupted (app killed / connection dropped) right as the 3rd
            // page's response arrives -- pages 1 and 2 are already committed
            // (with a checkpoint) by this point; this response never gets
            // processed because checkCancelled() fires first.
            if (speciesCalls == 3)
                m_builder->cancel();
            return pagedSpecies(req, kTotal);
        }
        return route(req);
    });

    ProjectBuilder::Request r = request();
    r.perPage = kPerPage;

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(r);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), false);   // "interrupted"
    const int projectId = spy.at(0).at(2).toInt();
    QVERIFY(projectId > 0);

    const auto checkpoint = m_store->buildCheckpoint(projectId);
    QVERIFY(checkpoint.has_value());
    QCOMPARE(checkpoint->nextPage, 3);   // only pages 1 and 2 made it in
    QVERIFY(m_store->projectTaxonInatIds(projectId).size() < kTotal);

    // "Relaunch": a fresh HttpClient/INatClient/ProjectBuilder (a new app
    // session), but the same TaxonomyStore/Database (the same catalogue).
    speciesCalls = 0;
    wire([&](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/observations/species_counts"))) {
            ++speciesCalls;
            return pagedSpecies(req, kTotal);
        }
        if (req.url.path().endsWith(QLatin1String("/taxa/800")))
            return singleGenusBatch();
        return route(req);
    });

    QSignalSpy resumeSpy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(r);
    QVERIFY(resumeSpy.wait(5000));
    QCOMPARE(resumeSpy.at(0).at(0).toBool(), true);
    QCOMPARE(resumeSpy.at(0).at(2).toInt(), projectId);

    // Resumed at page 2 (nextPage(3) - 1), not page 1 -- one page re-fetched
    // for boundary safety, not the whole list.
    QCOMPARE(speciesCalls, 3);   // pages 2, 3, 4

    // Every species made it in, including the last page's -- the seen/total
    // bookkeeping across the resume must not overcount and stop early.
    const auto finalIds = m_store->projectTaxonInatIds(projectId);
    for (int i = 0; i < kTotal; ++i)
        QVERIFY(finalIds.contains(100000 + i));
    // + family, kingdom, phylum, genus (800): the ancestor fill also covers
    // species that were only ever committed by the FIRST (interrupted) run,
    // via projectSpeciesAncestorIds() -- not just species this run fetched.
    QCOMPARE(finalIds.size(), kTotal + 4);

    QVERIFY(!m_store->buildCheckpoint(projectId).has_value());
}

void TestProjectBuilder::mismatchedCheckpointParamsAreDiscarded()
{
    wire([](const Transport::Request &req, int) { return route(req); });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const int projectId = spy.at(0).at(2).toInt();

    // A checkpoint left over from a build with a different page size --
    // must never be trusted for this run's resume.
    BuildCheckpoint stale;
    stale.rootTaxonInatId = 47217;
    stale.perPage = 999;
    stale.nextPage = 50;
    stale.speciesSeen = 49000;
    stale.speciesTotal = 50000;
    QVERIFY(m_store->saveBuildCheckpoint(projectId, stale));

    wire([](const Transport::Request &req, int) { return route(req); });
    QSignalSpy spy2(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy2.wait(5000));
    QCOMPARE(spy2.at(0).at(0).toBool(), true);
    QCOMPARE(spy2.at(0).at(2).toInt(), projectId);

    // Ran a normal full build from page 1 (route()'s fixed 2-species page),
    // not whatever a resume at the bogus nextPage=50 would have produced.
    QCOMPARE(m_store->projectTaxonInatIds(projectId).size(), 7);
    QVERIFY(!m_store->buildCheckpoint(projectId).has_value());
}

void TestProjectBuilder::checkpointClearedOnSuccessfulFinish()
{
    wire([](const Transport::Request &req, int) { return route(req); });

    QSignalSpy spy(m_builder.get(), &ProjectBuilder::finished);
    m_builder->start(request());
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const int projectId = spy.at(0).at(2).toInt();

    QVERIFY(!m_store->buildCheckpoint(projectId).has_value());
}

QTEST_GUILESS_MAIN(TestProjectBuilder)
#include "tst_projectbuilder.moc"
