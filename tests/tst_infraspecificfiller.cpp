#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/InfraspecificFiller.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

// A taxonomy /v1/taxa/{id} reply: the species with a `children` array.
static QByteArray taxonWithChildren(qint64 speciesId, const QByteArray &childrenJson)
{
    return "{\"results\":[{\"id\":" + QByteArray::number(speciesId)
           + ",\"rank\":\"species\",\"name\":\"Sp\",\"children\":[" + childrenJson + "]}]}";
}

class TestInfraspecificFiller : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void addsActiveInfraspecificChildrenAndMarksSpeciesChecked();
    void dropsChildrenWithNoObservationsInThePlace();
    void noPlaceProjectTakesEveryActiveChild();
    void rerunSkipsAlreadyCheckedSpecies();
    void scopedRunReChecksEvenAlreadyCheckedSpecies();
    void scopedRunOnlyChecksSpeciesUnderTheGivenTaxon();
    void fetchErrorFailsTheRun();
    void cancellationStopsTheRun();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<pl::net::INatClient> m_inat;
    std::unique_ptr<InfraspecificFiller> m_filler;

    int m_projectId = -1;         // worldwide "Orchids"
    int m_placedProjectId = -1;   // "Victorian Orchids", place 6744

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
};

void TestInfraspecificFiller::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());

    Taxon genus;
    genus.inatId = 800;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    for (qint64 id : {900, 901}) {
        Taxon sp;
        sp.inatId = id;
        sp.parentInatId = 800;
        sp.rank = QStringLiteral("species");
        sp.name = QStringLiteral("Diuris sp%1").arg(id);
        m_store->upsertTaxon(sp);
    }

    m_projectId = m_store->ensureProject(QStringLiteral("Orchids"), 800, std::nullopt,
                                         QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
    for (qint64 id : {800, 900, 901})
        QVERIFY(m_store->addProjectTaxon(m_projectId, id, true, false));

    Place vic;
    vic.inatId = 6744;
    vic.name = QStringLiteral("Victoria");
    QVERIFY(m_store->upsertPlace(vic));
    m_placedProjectId = m_store->ensureProject(QStringLiteral("Victorian Orchids"), 800,
                                               std::optional<qint64>(6744),
                                               QStringLiteral("inat"));
    QVERIFY(m_placedProjectId > 0);
    QVERIFY(m_store->addProjectTaxon(m_placedProjectId, 900, true, false));
}

void TestInfraspecificFiller::cleanup()
{
    m_filler.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

void TestInfraspecificFiller::wire(
    std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_http = std::make_unique<HttpClient>(std::move(owned), m_store.get());
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<pl::net::INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_filler = std::make_unique<InfraspecificFiller>(*m_inat, *m_store);
}

void TestInfraspecificFiller::addsActiveInfraspecificChildrenAndMarksSpeciesChecked()
{
    // Worldwide project -> no place filter, so every active child is taken and
    // the /observations endpoint is never hit.
    wire([](const Transport::Request &req, int) -> Transport::Reply {
        const QString path = req.url.path();
        if (path.endsWith(QLatin1String("/taxa/900")))
            return FakeTransport::ok(taxonWithChildren(900, R"(
                {"id":9001,"rank":"subspecies","name":"Diuris sp900 pardina","parent_id":900,"is_active":true},
                {"id":9002,"rank":"variety","name":"Diuris sp900 alba","parent_id":900,"is_active":true},
                {"id":9003,"rank":"subspecies","name":"Diuris sp900 extinct","parent_id":900,"is_active":false})"));
        if (path.endsWith(QLatin1String("/taxa/901")))
            return FakeTransport::ok(taxonWithChildren(901, QByteArray()));
        return FakeTransport::httpStatus(404);
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);   // ok
    QCOMPARE(spy.at(0).at(2).toInt(), 2);        // 2 active infraspecific children added

    const auto tree = m_store->projectTree(m_projectId);
    QCOMPARE(tree.size(), 5);   // genus + 2 species + 2 new infraspecific taxa

    QVERIFY(m_store->taxonInatIdByFoldedName(
                TaxonomyStore::foldName(QStringLiteral("Diuris sp900 pardina"))).has_value());
    QVERIFY(!m_store->taxonInatIdByFoldedName(
                 TaxonomyStore::foldName(QStringLiteral("Diuris sp900 extinct"))).has_value());

    QCOMPARE(m_store->projectSpeciesNeedingInfraCheck(m_projectId).size(), 0);
}

void TestInfraspecificFiller::dropsChildrenWithNoObservationsInThePlace()
{
    QStringList obsChecks;
    wire([&obsChecks](const Transport::Request &req, int) -> Transport::Reply {
        const QString url = req.url.toString();
        if (req.url.path().endsWith(QLatin1String("/taxa/900")))
            return FakeTransport::ok(taxonWithChildren(900, R"(
                {"id":9001,"rank":"subspecies","name":"D pardina here","parent_id":900,"is_active":true},
                {"id":9002,"rank":"subspecies","name":"D pardina elsewhere","parent_id":900,"is_active":true})"));
        if (req.url.path().endsWith(QLatin1String("/observations"))) {
            obsChecks << url;
            const int count = url.contains(QLatin1String("taxon_id=9001")) ? 12 : 0;
            return FakeTransport::ok("{\"total_results\":" + QByteArray::number(count) + "}");
        }
        return FakeTransport::httpStatus(404);
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_placedProjectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(2).toInt(), 1);        // only the in-region subspecies
    QCOMPARE(obsChecks.size(), 2);               // both candidates were region-checked
    for (const QString &u : obsChecks)
        QVERIFY2(u.contains(QLatin1String("place_id=6744")), qPrintable(u));

    QVERIFY(m_store->taxonInatIdByFoldedName(
                TaxonomyStore::foldName(QStringLiteral("D pardina here"))).has_value());
    QVERIFY(!m_store->taxonInatIdByFoldedName(
                 TaxonomyStore::foldName(QStringLiteral("D pardina elsewhere"))).has_value());
}

void TestInfraspecificFiller::noPlaceProjectTakesEveryActiveChild()
{
    int obsCalls = 0;
    wire([&obsCalls](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().endsWith(QLatin1String("/observations"))) {
            ++obsCalls;
            return FakeTransport::ok(QByteArrayLiteral("{\"total_results\":0}"));
        }
        if (req.url.path().endsWith(QLatin1String("/taxa/900")))
            return FakeTransport::ok(taxonWithChildren(900, R"(
                {"id":9001,"rank":"subspecies","name":"A","parent_id":900,"is_active":true},
                {"id":9002,"rank":"form","name":"B","parent_id":900,"is_active":true})"));
        return FakeTransport::ok(taxonWithChildren(901, QByteArray()));
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId);   // worldwide project
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(2).toInt(), 2);
    QCOMPARE(obsCalls, 0);   // no place -> no region checks
}

void TestInfraspecificFiller::rerunSkipsAlreadyCheckedSpecies()
{
    int taxaCalls = 0;
    wire([&taxaCalls](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/taxa/"))) {
            ++taxaCalls;
            const qint64 id = req.url.path().section(QLatin1Char('/'), -1).toLongLong();
            return FakeTransport::ok(taxonWithChildren(id, QByteArray()));
        }
        return FakeTransport::httpStatus(404);
    });

    QVERIFY(m_store->markInfraChecked(m_projectId, 900));

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId);   // blanket run
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(taxaCalls, 1);   // only the unchecked species (901)
}

void TestInfraspecificFiller::scopedRunReChecksEvenAlreadyCheckedSpecies()
{
    int taxaCalls = 0;
    wire([&taxaCalls](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/taxa/"))) {
            ++taxaCalls;
            const qint64 id = req.url.path().section(QLatin1Char('/'), -1).toLongLong();
            return FakeTransport::ok(taxonWithChildren(id, QByteArray()));
        }
        return FakeTransport::httpStatus(404);
    });

    QVERIFY(m_store->markInfraChecked(m_projectId, 900));

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId, 900);   // explicitly scoped to species 900
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(taxaCalls, 1);   // 900 re-checked despite its flag
}

void TestInfraspecificFiller::scopedRunOnlyChecksSpeciesUnderTheGivenTaxon()
{
    QStringList requested;
    wire([&requested](const Transport::Request &req, int) -> Transport::Reply {
        if (req.url.path().contains(QLatin1String("/taxa/"))) {
            const qint64 id = req.url.path().section(QLatin1Char('/'), -1).toLongLong();
            requested << QString::number(id);
            return FakeTransport::ok(taxonWithChildren(id, QByteArray()));
        }
        return FakeTransport::httpStatus(404);
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId, 901);   // scope: species 901 only
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(requested, QStringList{QStringLiteral("901")});
}

void TestInfraspecificFiller::fetchErrorFailsTheRun()
{
    wire([](const Transport::Request &, int) -> Transport::Reply {
        return FakeTransport::httpStatus(500);
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    QVERIFY(!m_filler->isRunning());
}

void TestInfraspecificFiller::cancellationStopsTheRun()
{
    wire([this](const Transport::Request &, int call) -> Transport::Reply {
        if (call == 0)
            m_filler->cancel();
        return FakeTransport::ok(taxonWithChildren(900, QByteArray()));
    });

    QSignalSpy spy(m_filler.get(), &InfraspecificFiller::finished);
    m_filler->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("cancelled"));
}

QTEST_GUILESS_MAIN(TestInfraspecificFiller)
#include "tst_infraspecificfiller.moc"
