#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/ReferencePhotoFetcher.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

class TestReferencePhotoFetcher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void fillsMissingPhotosAndSkipsThoseAlreadyCached();
    void fetchErrorFailsTheRun();
    void cancellationStopsTheRun();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<pl::net::INatClient> m_inat;
    std::unique_ptr<ReferencePhotoFetcher> m_fetcher;

    int m_projectId = -1;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    QString photoUrlOf(qint64 inatId) const;
};

void TestReferencePhotoFetcher::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());

    Taxon genus;
    genus.inatId = 800;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    for (qint64 id : {900, 901, 902}) {
        Taxon sp;
        sp.inatId = id;
        sp.parentInatId = 800;
        sp.rank = QStringLiteral("species");
        sp.name = QStringLiteral("Diuris sp%1").arg(id);
        m_store->upsertTaxon(sp);
    }

    // 902 already has a photo -> it should not be re-fetched.
    Taxon has = {};
    has.inatId = 902;
    has.parentInatId = 800;
    has.rank = QStringLiteral("species");
    has.name = QStringLiteral("Diuris sp902");
    has.photoUrl = QStringLiteral("https://inat.example/photos/existing/medium.jpg");
    m_store->upsertTaxon(has);

    m_projectId = m_store->ensureProject(QStringLiteral("Diuris"), 800, std::nullopt,
                                         QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
    for (qint64 id : {800, 900, 901, 902})
        QVERIFY(m_store->addProjectTaxon(m_projectId, id, true, false));
}

void TestReferencePhotoFetcher::cleanup()
{
    m_fetcher.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

void TestReferencePhotoFetcher::wire(
    std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_http = std::make_unique<HttpClient>(std::move(owned), m_store.get());
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<pl::net::INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_fetcher = std::make_unique<ReferencePhotoFetcher>(*m_inat, *m_store);
}

QString TestReferencePhotoFetcher::photoUrlOf(qint64 inatId) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT photo_url FROM taxon WHERE inat_id = ?"));
    q.addBindValue(inatId);
    return (q.exec() && q.next()) ? q.value(0).toString() : QString();
}

void TestReferencePhotoFetcher::fillsMissingPhotosAndSkipsThoseAlreadyCached()
{
    QString capturedPath;
    wire([&capturedPath](const Transport::Request &req, int) -> Transport::Reply {
        capturedPath = req.url.path();
        return FakeTransport::ok(R"({"results":[
            {"id":900,"rank":"species","name":"Diuris sp900",
             "default_photo":{"medium_url":"https://inat.example/photos/900/medium.jpg",
                              "attribution":"(c) a"}},
            {"id":901,"rank":"species","name":"Diuris sp901",
             "default_photo":{"medium_url":"https://inat.example/photos/901/medium.jpg"}}
        ]})");
    });

    QSignalSpy spy(m_fetcher.get(), &ReferencePhotoFetcher::finished);
    m_fetcher->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);   // ok
    QCOMPARE(spy.at(0).at(2).toInt(), 2);        // 2 taxa updated

    // Only the two photoless species were requested (902 was skipped).
    QVERIFY(capturedPath.endsWith(QLatin1String("/taxa/900,901")));

    QCOMPARE(photoUrlOf(900), QStringLiteral("https://inat.example/photos/900/medium.jpg"));
    QCOMPARE(photoUrlOf(901), QStringLiteral("https://inat.example/photos/901/medium.jpg"));
    QCOMPARE(photoUrlOf(902), QStringLiteral("https://inat.example/photos/existing/medium.jpg"));

    // Re-run: nothing left to do.
    QVERIFY(m_store->projectLeafTaxaMissingPhoto(m_projectId).isEmpty());
}

void TestReferencePhotoFetcher::fetchErrorFailsTheRun()
{
    wire([](const Transport::Request &, int) -> Transport::Reply {
        return FakeTransport::httpStatus(500);
    });

    QSignalSpy spy(m_fetcher.get(), &ReferencePhotoFetcher::finished);
    m_fetcher->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    QVERIFY(!m_fetcher->isRunning());
}

void TestReferencePhotoFetcher::cancellationStopsTheRun()
{
    wire([this](const Transport::Request &, int call) -> Transport::Reply {
        if (call == 0)
            m_fetcher->cancel();
        return FakeTransport::ok(R"({"results":[]})");
    });

    QSignalSpy spy(m_fetcher.get(), &ReferencePhotoFetcher::finished);
    m_fetcher->start(m_projectId);
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("cancelled"));
}

QTEST_GUILESS_MAIN(TestReferencePhotoFetcher)
#include "tst_referencephotofetcher.moc"
