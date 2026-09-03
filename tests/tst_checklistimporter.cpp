#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrlQuery>

#include "FakeTransport.h"
#include "checklist/ChecklistImporter.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::checklist;

namespace {

Transport::Reply route(const Transport::Request &req)
{
    const QString path = req.url.path();
    const QString q = QUrlQuery(req.url).queryItemValue(QStringLiteral("q"));

    if (path.endsWith(QLatin1String("/taxa")) && !q.isEmpty()) {
        if (q == QLatin1String("Diuris pardina"))
            return FakeTransport::ok(R"({"results":[{"id":900,"rank":"species","rank_level":10,"name":"Diuris pardina","is_active":true,"ancestry":"48460/47217/100"}]})");
        if (q == QLatin1String("Diuris sulphurea"))
            return FakeTransport::ok(R"({"results":[{"id":901,"rank":"species","rank_level":10,"name":"Diuris sulphurea","is_active":true,"ancestry":"48460/47217/100"}]})");
        if (q == QLatin1String("Diuris amabilis"))  // checklist spelling; iNat says "Diuris punctata"
            return FakeTransport::ok(R"({"results":[{"id":902,"rank":"species","rank_level":10,"name":"Diuris punctata","is_active":true,"ancestry":"48460/47217/100"}]})");
        return FakeTransport::ok(R"({"results":[]})");  // unresolved
    }
    if (path.contains(QLatin1String("/taxa/"))) {  // ancestor batch fill
        return FakeTransport::ok(R"({"results":[{"id":48460,"rank":"kingdom","rank_level":70,"name":"Plantae"},{"id":47217,"rank":"family","rank_level":30,"name":"Orchidaceae"}]})");
    }
    return FakeTransport::httpStatus(404);
}

} // namespace

class TestChecklistImporter : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void importsStatusesAndLinksToProject();
    void skipsRowsOutsideProjectGenera();
    void keepsChecklistSpellingAsSynonym();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<taxonomy::TaxonomyStore> m_store;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<ChecklistImporter> m_importer;
    int m_projectId = -1;

    void wire();
    int scalar(const QString &sql);
    static void await(QSignalSpy &spy)
    {
        if (spy.isEmpty())
            QVERIFY(spy.wait(5000));
    }
};

void TestChecklistImporter::wire()
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [](const Transport::Request &r, int) { return route(r); };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), m_store.get());
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_importer = std::make_unique<ChecklistImporter>(*m_inat, *m_store);
}

void TestChecklistImporter::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<taxonomy::TaxonomyStore>(m_db->connectionName());

    taxonomy::Taxon genus;
    genus.inatId = 100;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);
    m_projectId = m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 7830,
                                         QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
    m_store->addProjectTaxon(m_projectId, 100, false, false);

    wire();
}

void TestChecklistImporter::cleanup()
{
    m_importer.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

int TestChecklistImporter::scalar(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

// finished(bool ok, QString error, int imported, int skipped, int unresolved)
void TestChecklistImporter::importsStatusesAndLinksToProject()
{
    ChecklistImporter::Request req;
    req.projectId = m_projectId;
    req.source = QStringLiteral("VBA 2021");
    req.placeInatId = 7830;
    req.entries = {
        {QStringLiteral("Diuris pardina"), QStringLiteral("Rare"), QStringLiteral("rare")},
        {QStringLiteral("Diuris sulphurea"), QString(), QString()},
        {QStringLiteral("Diuris ghostus"), QStringLiteral("Extinct"), QStringLiteral("extinct")},
        {QStringLiteral("Acacia dealbata"), QStringLiteral("Vulnerable"), QStringLiteral("vulnerable")},
    };

    QSignalSpy spy(m_importer.get(), &ChecklistImporter::finished);
    m_importer->start(req);
    await(spy);

    QCOMPARE(spy.at(0).at(0).toBool(), true);   // ok
    QCOMPARE(spy.at(0).at(2).toInt(), 2);       // imported
    QCOMPARE(spy.at(0).at(3).toInt(), 1);       // skipped (Acacia)
    QCOMPARE(spy.at(0).at(4).toInt(), 1);       // unresolved (Diuris ghostus)

    QCOMPARE(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM project_taxon WHERE project_id = %1 AND from_checklist = 1")
                        .arg(m_projectId)), 2);

    QSqlQuery s(QSqlDatabase::database(m_db->connectionName(), false));
    s.exec(QStringLiteral(
        "SELECT ts.status, ts.source FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
        "WHERE t.inat_id = 900"));
    QVERIFY(s.next());
    QCOMPARE(s.value(0).toString(), QStringLiteral("Rare"));
    QCOMPARE(s.value(1).toString(), QStringLiteral("VBA 2021"));

    QVERIFY(m_store->taxonLocalId(48460).has_value());
    QVERIFY(m_store->taxonLocalId(47217).has_value());
}

void TestChecklistImporter::skipsRowsOutsideProjectGenera()
{
    ChecklistImporter::Request req;
    req.projectId = m_projectId;
    req.entries = {
        {QStringLiteral("Eucalyptus obliqua"), QStringLiteral("Rare"), QStringLiteral("rare")},
        {QStringLiteral("Banksia marginata"), QString(), QString()},
    };

    QSignalSpy spy(m_importer.get(), &ChecklistImporter::finished);
    m_importer->start(req);
    await(spy);

    QCOMPARE(spy.at(0).at(2).toInt(), 0);   // imported
    QCOMPARE(spy.at(0).at(3).toInt(), 2);   // skipped
    QCOMPARE(m_transport->received.size(), 0);
}

void TestChecklistImporter::keepsChecklistSpellingAsSynonym()
{
    ChecklistImporter::Request req;
    req.projectId = m_projectId;
    req.source = QStringLiteral("VBA 2021");
    req.entries = {
        {QStringLiteral("Diuris amabilis"), QStringLiteral("Endangered"),
         QStringLiteral("endangered")},
    };

    QSignalSpy spy(m_importer.get(), &ChecklistImporter::finished);
    m_importer->start(req);
    await(spy);

    QCOMPARE(spy.at(0).at(2).toInt(), 1);   // imported

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral(
        "SELECT kind FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
        "WHERE t.inat_id = 902 AND tn.name_folded = 'diuris amabilis'"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("synonym"));
}

QTEST_GUILESS_MAIN(TestChecklistImporter)
#include "tst_checklistimporter.moc"
