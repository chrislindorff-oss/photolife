#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "taxonomy/AlaStatusSync.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

namespace {

QJsonObject item(const QString &name, const QString &status, const QString &matched = {})
{
    QJsonObject it;
    it.insert(QStringLiteral("name"), name);
    it.insert(QStringLiteral("scientificName"), matched.isEmpty() ? name : matched);
    it.insert(QStringLiteral("kvpValues"),
              QJsonArray{QJsonObject{{QStringLiteral("key"), QStringLiteral("status")},
                                     {QStringLiteral("value"), status}}});
    return it;
}

QByteArray lists()
{
    auto list = [](const QString &id, const QString &name, const QString &region, bool auth) {
        return QJsonObject{{QStringLiteral("dataResourceUid"), id},
                           {QStringLiteral("listName"), name},
                           {QStringLiteral("region"), region},
                           {QStringLiteral("isAuthoritative"), auth}};
    };
    const QJsonArray all{
        list(QStringLiteral("dr655"), QStringLiteral("Victoria : Conservation Status"),
             QStringLiteral("Victoria"), true),
        list(QStringLiteral("dr7904"), QStringLiteral("ACT Rare Plant List"), QString(), false),
        list(QStringLiteral("dr2286"), QStringLiteral("NZ plants"), QStringLiteral("NZ"), true),
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("lists"), all}}).toJson();
}

} // namespace

class TestAlaStatusSync : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void recordsAuthoritativeStateListsForCachedTaxa();
    void mapsRegionsToINaturalistPlaces();
    void cancelStopsTheRun();
    void rematchesFromKeptListsWithoutDownloading();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    FakeTransport *m_transport = nullptr;
    int scalar(const QString &sql);
};

void TestAlaStatusSync::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
    auto add = [&](qint64 id, const QString &rank, const QString &name,
                   const QStringList &synonyms = {}) {
        Taxon t;
        t.inatId = id;
        t.rank = rank;
        t.name = name;
        t.synonyms = synonyms;
        m_store->upsertTaxon(t);
    };
    add(1, QStringLiteral("species"), QStringLiteral("Caladenia amoena"));
    add(2, QStringLiteral("subspecies"), QStringLiteral("Abutilon fraseri diplotrichum"));
    add(3, QStringLiteral("species"), QStringLiteral("Petalochilus amoenus"));   // unrelated
    add(4, QStringLiteral("species"), QStringLiteral("Glossodia major"),
        {QStringLiteral("Glossodia minor")});
}

void TestAlaStatusSync::cleanup()
{
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

int TestAlaStatusSync::scalar(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

void TestAlaStatusSync::recordsAuthoritativeStateListsForCachedTaxa()
{
    // Page 1 is exactly a full page, so a second page is fetched.
    QJsonArray page1;
    page1.append(item(QStringLiteral("Caladenia amoena"), QStringLiteral("Critically Endangered")));
    page1.append(item(QStringLiteral("Abutilon fraseri subsp. diplotrichum"),
                      QStringLiteral("Endangered")));
    // Listed under an older name; ALA's matched name is the one we know.
    page1.append(item(QStringLiteral("Glossodia minor"), QStringLiteral("Vulnerable"),
                      QStringLiteral("Glossodia major")));
    while (page1.size() < AlaStatusSync::kPageSize)
        page1.append(item(QStringLiteral("Unknown species %1").arg(page1.size()),
                          QStringLiteral("Endangered")));
    const QJsonArray page2{item(QStringLiteral("Not cached either"), QStringLiteral("Vulnerable"))};

    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [&](const Transport::Request &r, int) {
        const QString url = r.url.toString();
        if (url.contains(QStringLiteral("/speciesList?")))
            return FakeTransport::ok(lists());
        if (url.contains(QStringLiteral("speciesListItems/dr655"))
            && url.contains(QStringLiteral("offset=0")))
            return FakeTransport::ok(QJsonDocument(page1).toJson());
        if (url.contains(QStringLiteral("speciesListItems/dr655")))
            return FakeTransport::ok(QJsonDocument(page2).toJson());
        return FakeTransport::httpStatus(404);
    };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);

    AlaStatusSync sync(*m_http, *m_store);
    sync.setBaseUrl(QStringLiteral("https://api.test/specieslist/ws"));
    QSignalSpy spy(&sync, &AlaStatusSync::finished);
    QSignalSpy progress(&sync, &AlaStatusSync::progress);
    sync.start();
    QVERIFY(spy.wait(10000));

    QVERIFY2(spy.at(0).at(0).toBool(), qPrintable(spy.at(0).at(1).toString()));
    QCOMPARE(progress.size(), 1);   // one list
    QCOMPARE(progress.at(0).at(0).toInt(), 0);
    QCOMPARE(progress.at(0).at(1).toInt(), 1);
    QCOMPARE(progress.at(0).at(2).toString(), QStringLiteral("Victoria : Conservation Status"));
    QCOMPARE(spy.at(0).at(2).toInt(), 1);   // only the authoritative, mapped list
    QCOMPARE(spy.at(0).at(3).toInt(), 3);
    QCOMPARE(m_transport->received.size(), 3);   // lists + two pages
    QVERIFY(m_transport->received.at(2).url.toString().contains(QStringLiteral("offset=1000")));

    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
                 "WHERE t.inat_id = 1 AND ts.status = 'Critically Endangered' "
                 "AND ts.place_inat_id = 7830 AND ts.source = 'ALA dr655'")),
             1);
    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
                 "WHERE t.inat_id = 2")),
             1);   // matched through the "subsp." connector
    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
                 "WHERE t.inat_id = 4 AND ts.status = 'Vulnerable'")),
             1);
    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
                 "WHERE t.inat_id = 3")),
             0);

    // A re-run replaces the list's statuses rather than adding to them.
    sync.start();
    QVERIFY(spy.wait(10000));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM taxon_status")), 3);
}

void TestAlaStatusSync::mapsRegionsToINaturalistPlaces()
{
    QCOMPARE(AlaStatusSync::placeForRegion(QStringLiteral("Victoria")).value_or(0), qint64(7830));
    QCOMPARE(AlaStatusSync::placeForRegion(QStringLiteral("Australia")).value_or(0), qint64(6744));
    QCOMPARE(AlaStatusSync::placeForRegion(QStringLiteral("New South Wales")).value_or(0),
             qint64(6825));
    QCOMPARE(AlaStatusSync::placeForRegion(QStringLiteral(" western australia ")).value_or(0),
             qint64(6827));
    QVERIFY(!AlaStatusSync::placeForRegion(QStringLiteral("NZ")));
    QVERIFY(!AlaStatusSync::placeForRegion(QString()));
}

void TestAlaStatusSync::cancelStopsTheRun()
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [](const Transport::Request &, int) { return FakeTransport::ok(lists()); };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);

    AlaStatusSync sync(*m_http, *m_store);
    sync.setBaseUrl(QStringLiteral("https://api.test/specieslist/ws"));
    QSignalSpy spy(&sync, &AlaStatusSync::finished);
    sync.start();
    sync.cancel();
    QVERIFY(spy.wait(5000));
    QVERIFY(!spy.at(0).at(0).toBool());
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    QVERIFY(!sync.isRunning());
    QCOMPARE(m_transport->received.size(), 1);   // no list items were fetched
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM taxon_status")), 0);
}

void TestAlaStatusSync::rematchesFromKeptListsWithoutDownloading()
{
    const QJsonArray page{item(QStringLiteral("Caladenia amoena"), QStringLiteral("Endangered")),
                          item(QStringLiteral("Pterostylis newcomer"), QStringLiteral("Vulnerable"))};
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [&](const Transport::Request &r, int) {
        if (r.url.toString().contains(QStringLiteral("/speciesList?")))
            return FakeTransport::ok(lists());
        return FakeTransport::ok(QJsonDocument(page).toJson());
    };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AlaStatusSync sync(*m_http, *m_store, dir.path());
    sync.setBaseUrl(QStringLiteral("https://api.test/specieslist/ws"));
    QSignalSpy spy(&sync, &AlaStatusSync::finished);

    sync.start();   // nothing kept yet: downloads, and keeps the lists
    QVERIFY(spy.wait(10000));
    QVERIFY2(spy.at(0).at(0).toBool(), qPrintable(spy.at(0).at(1).toString()));
    QCOMPARE(m_transport->received.size(), 2);   // lists + one page
    QVERIFY(QFile::exists(sync.cachePath()));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM taxon_status")), 1);

    // A new tree brings a listed species: re-matched from the kept copy.
    Taxon t;
    t.inatId = 5;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Pterostylis newcomer");
    m_store->upsertTaxon(t);
    sync.start();
    QVERIFY(spy.wait(10000));
    QVERIFY2(spy.at(1).at(0).toBool(), qPrintable(spy.at(1).at(1).toString()));
    QCOMPARE(spy.at(1).at(2).toInt(), 1);
    QCOMPARE(spy.at(1).at(3).toInt(), 2);
    QCOMPARE(m_transport->received.size(), 2);   // no requests
    QCOMPARE(scalar(QStringLiteral(
                 "SELECT COUNT(*) FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
                 "WHERE t.inat_id = 5 AND ts.status = 'Vulnerable' AND ts.place_inat_id = 7830")),
             1);

    sync.start(true);   // forced: downloads again
    QVERIFY(spy.wait(10000));
    QVERIFY(spy.at(2).at(0).toBool());
    QCOMPARE(m_transport->received.size(), 4);
}

QTEST_GUILESS_MAIN(TestAlaStatusSync)
#include "tst_alastatussync.moc"
