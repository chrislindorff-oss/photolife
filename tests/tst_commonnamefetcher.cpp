#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/CommonNameFetcher.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

class TestCommonNameFetcher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void fetchesEnglishNamesAndRemembersWhatItFetched();
    void keepsTheUsersOwnNames();
    void fetchErrorFailsTheRun();
    void photosTaxaComeFirst();

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<CommonNameFetcher> m_fetcher;
    QStringList m_requests;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    QStringList commonNamesOf(qint64 inatId) const;
};

void TestCommonNameFetcher::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
    for (const auto &[id, name, common] :
         {std::tuple{qint64(8000), "Porphyrio melanotus", "Australasian Swamphen"},
          std::tuple{qint64(8001), "Petroica phoenicea", "Flame Robin"}}) {
        Taxon t;
        t.inatId = id;
        t.rank = QStringLiteral("species");
        t.name = QLatin1String(name);
        t.commonName = QLatin1String(common);
        t.vernacular = {t.commonName};
        m_store->upsertTaxon(t);
    }
    const int pid = m_store->ensureProject(QStringLiteral("Birds"), 8000, std::nullopt,
                                           QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(pid, 8000, true, false));
    QVERIFY(m_store->addProjectTaxon(pid, 8001, true, false));
    m_requests.clear();
}

void TestCommonNameFetcher::cleanup()
{
    m_fetcher.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
    QFile::remove(m_dir.filePath(QStringLiteral("common_names.db")));
}

void TestCommonNameFetcher::wire(
    std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [this, responder](const Transport::Request &req, int call) {
        m_requests << req.url.toString();
        return responder(req, call);
    };
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_fetcher = std::make_unique<CommonNameFetcher>(
        *m_inat, *m_store, m_dir.filePath(QStringLiteral("common_names.db")));
}

QStringList TestCommonNameFetcher::commonNamesOf(qint64 inatId) const
{
    QStringList out;
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT tn.name FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
                             "WHERE t.inat_id = ? AND tn.kind IN ('vernacular', 'user-vernacular') "
                             "ORDER BY tn.name"));
    q.addBindValue(inatId);
    if (q.exec()) {
        while (q.next())
            out << q.value(0).toString();
    }
    return out;
}

namespace {
const char *kReply = R"({"results":[
    {"id":8000,"rank":"species","name":"Porphyrio melanotus",
     "preferred_common_name":"Australasian Swamphen","names":[
        {"name":"Porphyrio melanotus","lexicon":"scientific-names"},
        {"name":"Australasian Swamphen","lexicon":"english","locale":"en"},
        {"name":"Purple Swamphen","lexicon":"english","locale":"en"},
        {"name":"Pukeko","lexicon":"maori","locale":"mi"}]},
    {"id":8001,"rank":"species","name":"Petroica phoenicea",
     "preferred_common_name":"Flame Robin","names":[
        {"name":"Petroica phoenicea","lexicon":"scientific-names"},
        {"name":"Flame Robin","lexicon":"english","locale":"en"}]}
]})";
} // namespace

void TestCommonNameFetcher::fetchesEnglishNamesAndRemembersWhatItFetched()
{
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kReply); });
    QCOMPARE(m_fetcher->due().size(), 2);

    QSignalSpy spy(m_fetcher.get(), &CommonNameFetcher::finished);
    m_fetcher->start();
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(2).toInt(), 1);   // Purple Swamphen
    QCOMPARE(m_requests.size(), 1);
    QVERIFY(m_requests.first().contains(QLatin1String("/taxa/8000,8001")));
    QVERIFY(m_requests.first().contains(QLatin1String("all_names=true")));
    QCOMPARE(commonNamesOf(8000),
             (QStringList{QStringLiteral("Australasian Swamphen"), QStringLiteral("Purple Swamphen")}));

    // Fetched: not asked again (until kRefreshDays), unless forced.
    QVERIFY(m_fetcher->due().isEmpty());
    QCOMPARE(m_fetcher->due(CommonNameFetcher::Scope::All, true).size(), 2);
    spy.clear();
    m_fetcher->start();
    QVERIFY(spy.count() == 1 || spy.wait(5000));
    QCOMPARE(m_requests.size(), 1);

    // A later lean update (a species list) keeps them.
    Taxon lean;
    lean.inatId = 8000;
    lean.rank = QStringLiteral("species");
    lean.name = QStringLiteral("Porphyrio melanotus");
    lean.commonName = QStringLiteral("Australasian Swamphen");
    lean.vernacular = {lean.commonName};
    m_store->upsertTaxon(lean);
    QCOMPARE(commonNamesOf(8000).size(), 2);
}

void TestCommonNameFetcher::keepsTheUsersOwnNames()
{
    QVERIFY(m_store->addName(8001, QStringLiteral("Robin Redbreast"), TaxonomyStore::kUserVernacular));
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kReply); });
    QSignalSpy spy(m_fetcher.get(), &CommonNameFetcher::finished);
    m_fetcher->start();
    QVERIFY(spy.wait(5000));
    QVERIFY(commonNamesOf(8001).contains(QStringLiteral("Robin Redbreast")));
}

void TestCommonNameFetcher::fetchErrorFailsTheRun()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(500); });
    QSignalSpy spy(m_fetcher.get(), &CommonNameFetcher::finished);
    m_fetcher->start();
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QCOMPARE(m_fetcher->due().size(), 2);   // nothing recorded as fetched
}

// The taxa photos are matched to can be fetched on their own, and lead the
// full list.
void TestCommonNameFetcher::photosTaxaComeFirst()
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/p', 'p', 0)")));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO capture (id, folder_id, base_name) VALUES (1, 1, 'IMG_1')")));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                                  "SELECT 1, id, 'folder', 1.0, 'auto' FROM taxon WHERE inat_id = 8001")));
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kReply); });
    QCOMPARE(m_fetcher->due(CommonNameFetcher::Scope::PhotoTaxa), QList<qint64>{8001});
    QCOMPARE(m_fetcher->due(CommonNameFetcher::Scope::All), (QList<qint64>{8001, 8000}));
}

QTEST_GUILESS_MAIN(TestCommonNameFetcher)
#include "tst_commonnamefetcher.moc"
