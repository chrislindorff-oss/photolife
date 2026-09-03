#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::taxonomy;

class TestTaxonomyStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void foldNameNormalises();
    void placeRoundTrips();
    void taxonUpsertStoresNamesAndIsIdempotent();
    void reUpsertReplacesNamesWithoutDuplicating();
    void projectMembershipAndTree();
    void httpCacheRoundTrips();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
};

void TestTaxonomyStore::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
    QCOMPARE(m_db->schemaVersion(), Database::targetSchemaVersion());
    QVERIFY(Database::targetSchemaVersion() >= 2);
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
}

void TestTaxonomyStore::cleanup()
{
    m_store.reset();
    m_db.reset();
}

void TestTaxonomyStore::foldNameNormalises()
{
    QCOMPARE(TaxonomyStore::foldName(QStringLiteral("  Diuris   pardina ")),
             QStringLiteral("diuris pardina"));
    QCOMPARE(TaxonomyStore::foldName(QString::fromUtf8("Cl\xC3\xA4""dia")),
             QStringLiteral("cladia"));
    QCOMPARE(TaxonomyStore::foldName(QString::fromUtf8("Acacia \xC3\x97 grayana")),
             QString::fromUtf8("acacia \xC3\x97 grayana"));  // multiplication sign kept
}

void TestTaxonomyStore::placeRoundTrips()
{
    Place p;
    p.inatId = 6744;
    p.name = QStringLiteral("Victoria");
    p.displayName = QStringLiteral("Victoria, AU");
    p.adminLevel = 10;
    p.bboxSwLat = -39.2;
    QVERIFY(m_store->upsertPlace(p));

    const auto got = m_store->placeByInatId(6744);
    QVERIFY(got.has_value());
    QCOMPARE(got->name, QStringLiteral("Victoria"));
    QCOMPARE(got->adminLevel.value_or(-1), 10);

    p.displayName = QStringLiteral("State of Victoria");
    QVERIFY(m_store->upsertPlace(p));
    QCOMPARE(m_store->placeByInatId(6744)->displayName, QStringLiteral("State of Victoria"));
}

void TestTaxonomyStore::taxonUpsertStoresNamesAndIsIdempotent()
{
    Taxon t;
    t.inatId = 47217;
    t.rank = QStringLiteral("family");
    t.rankLevel = 30;
    t.name = QStringLiteral("Orchidaceae");
    t.commonName = QStringLiteral("Orchids");
    t.ancestry = QStringLiteral("48460/47126/211194");
    t.synonyms = {QStringLiteral("Orchidacées")};
    t.vernacular = {QStringLiteral("Orchids"), QStringLiteral("Orchid family")};

    const int id1 = m_store->upsertTaxon(t);
    QVERIFY(id1 > 0);
    const int id2 = m_store->upsertTaxon(t);
    QCOMPARE(id2, id1);

    QCOMPARE(m_store->taxonCount(), 1);
    QCOMPARE(m_store->taxonLocalId(47217).value_or(-1), qint64(id1));

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM taxon_name WHERE taxon_id = ?"));
    q.addBindValue(id1);
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 4);  // accepted + 1 synonym + 2 vernacular

    q.prepare(QStringLiteral(
        "SELECT kind FROM taxon_name WHERE taxon_id = ? AND name_folded = 'orchidaceae'"));
    q.addBindValue(id1);
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("accepted"));
}

void TestTaxonomyStore::reUpsertReplacesNamesWithoutDuplicating()
{
    Taxon t;
    t.inatId = 100;
    t.rank = QStringLiteral("genus");
    t.name = QStringLiteral("Caladenia");
    t.synonyms = {QStringLiteral("Petalochilus"), QStringLiteral("Arachnorchis")};
    const int id = m_store->upsertTaxon(t);

    t.synonyms = {QStringLiteral("Stegostyla")};  // fewer, different
    m_store->upsertTaxon(t);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM taxon_name WHERE taxon_id = ?"));
    q.addBindValue(id);
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 2);  // accepted + 1 synonym, old synonyms gone
}

void TestTaxonomyStore::projectMembershipAndTree()
{
    Taxon fam;
    fam.inatId = 47217;
    fam.rank = QStringLiteral("family");
    fam.rankLevel = 30;
    fam.name = QStringLiteral("Orchidaceae");
    m_store->upsertTaxon(fam);

    Taxon genus;
    genus.inatId = 60815;
    genus.parentInatId = 47217;
    genus.rank = QStringLiteral("genus");
    genus.rankLevel = 20;
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    Taxon sp;
    sp.inatId = 200000;
    sp.parentInatId = 60815;
    sp.rank = QStringLiteral("species");
    sp.rankLevel = 10;
    sp.name = QStringLiteral("Diuris pardina");
    m_store->upsertTaxon(sp);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 6744,
                                            QStringLiteral("inat"));
    QVERIFY(proj > 0);
    QCOMPARE(m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 6744,
                                    QStringLiteral("inat")),
             proj);

    QVERIFY(m_store->addProjectTaxon(proj, 47217, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200000, true, false));
    QVERIFY(!m_store->addProjectTaxon(proj, 999999, true, false));  // unknown taxon

    const auto ids = m_store->projectTaxonInatIds(proj);
    QCOMPARE(ids.size(), 3);

    const auto tree = m_store->projectTree(proj);
    QCOMPARE(tree.size(), 3);
    QCOMPARE(tree.at(0).rank, QStringLiteral("family"));   // highest rank_level first
    QCOMPARE(tree.at(2).name, QStringLiteral("Diuris pardina"));
    QVERIFY(tree.at(2).inRegion);
    QVERIFY(tree.at(2).isLeafRank);
    QVERIFY(!tree.at(0).isLeafRank);
}

void TestTaxonomyStore::httpCacheRoundTrips()
{
    const QString url = QStringLiteral("https://api.inaturalist.org/v1/taxa/47217");
    QVERIFY(!m_store->cachedResponse(url).has_value());

    QVERIFY(m_store->storeResponse(url, QStringLiteral("\"abc123\""), QString(), 200,
                                   QByteArrayLiteral("{\"results\":[]}")));
    auto e = m_store->cachedResponse(url);
    QVERIFY(e.has_value());
    QCOMPARE(e->etag, QStringLiteral("\"abc123\""));
    QCOMPARE(e->body, QByteArrayLiteral("{\"results\":[]}"));
    QCOMPARE(e->status, 200);

    QVERIFY(m_store->storeResponse(url, QStringLiteral("\"def456\""), QString(), 200,
                                   QByteArrayLiteral("{\"results\":[1]}")));
    QCOMPARE(m_store->cachedResponse(url)->etag, QStringLiteral("\"def456\""));
}

QTEST_GUILESS_MAIN(TestTaxonomyStore)
#include "tst_taxonomystore.moc"
