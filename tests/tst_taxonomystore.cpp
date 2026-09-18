#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

#include "PgTestDsn.h"

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
    void projectLocalityBoxResolvesFromPlace();
    void taxonPhotoPersistsAndSurvivesPhotolessUpsert();
    void projectLeafPhotosListAndMissing();
    void deleteProjectRemovesItAndItsMembership();
    void httpCacheRoundTrips();
    void pgAddProjectTaxonMergesFlagsOnConflict();

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
    QCOMPARE(got->bboxSwLat.value_or(0.0), -39.2);
    QVERIFY(!got->bboxSwLng.has_value());   // only bboxSwLat was set above
    QVERIFY(!got->bboxNeLat.has_value());
    QVERIFY(!got->bboxNeLng.has_value());

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

    // A genus-level hybrid formula: a real, identifiable leaf taxon in its own right,
    // not a subdivision of any species above.
    Taxon genusHybrid;
    genusHybrid.inatId = 300000;
    genusHybrid.parentInatId = 60815;
    genusHybrid.rank = QStringLiteral("genushybrid");
    genusHybrid.rankLevel = 20;
    genusHybrid.name = QStringLiteral("Diuris × palachila");
    m_store->upsertTaxon(genusHybrid);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 6744,
                                            QStringLiteral("inat"));
    QVERIFY(proj > 0);
    QCOMPARE(m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 6744,
                                    QStringLiteral("inat")),
             proj);

    QVERIFY(m_store->addProjectTaxon(proj, 47217, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200000, true, false));
    QVERIFY(m_store->addProjectTaxon(proj, 300000, true, false));
    QVERIFY(!m_store->addProjectTaxon(proj, 999999, true, false));  // unknown taxon

    const auto ids = m_store->projectTaxonInatIds(proj);
    QCOMPARE(ids.size(), 4);

    const auto tree = m_store->projectTree(proj);
    QCOMPARE(tree.size(), 4);
    QCOMPARE(tree.at(0).rank, QStringLiteral("family"));   // highest rank_level first
    QVERIFY(!tree.at(0).isLeafRank);

    const auto speciesIt = std::find_if(tree.begin(), tree.end(), [](const auto &n) {
        return n.inatId == 200000;
    });
    QVERIFY(speciesIt != tree.end());
    QVERIFY(speciesIt->inRegion);
    QVERIFY(speciesIt->isLeafRank);

    const auto hybridIt = std::find_if(tree.begin(), tree.end(), [](const auto &n) {
        return n.inatId == 300000;
    });
    QVERIFY(hybridIt != tree.end());
    QVERIFY(hybridIt->isLeafRank);   // genushybrid is a leaf, same as hybrid/subspecies/variety
}

void TestTaxonomyStore::projectLocalityBoxResolvesFromPlace()
{
    Place place;
    place.inatId = 6744;
    place.name = QStringLiteral("Victoria");
    place.displayName = QStringLiteral("Victoria, AU");
    place.bboxSwLat = -39.2;
    place.bboxSwLng = 140.9;
    place.bboxNeLat = -33.9;
    place.bboxNeLng = 150.0;
    QVERIFY(m_store->upsertPlace(place));

    const int withBbox =
        m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 47217, 6744,
                               QStringLiteral("inat"));
    QVERIFY(withBbox > 0);

    const auto box = m_store->projectLocalityBox(withBbox);
    QVERIFY(box.has_value());
    QCOMPARE(box->swLat, -39.2);
    QCOMPARE(box->swLng, 140.9);
    QCOMPARE(box->neLat, -33.9);
    QCOMPARE(box->neLng, 150.0);

    // A place with no bbox at all -> nullopt.
    Place placeWithoutBbox;
    placeWithoutBbox.inatId = 9999;
    placeWithoutBbox.name = QStringLiteral("Somewhere");
    QVERIFY(m_store->upsertPlace(placeWithoutBbox));
    const int noBboxProject =
        m_store->ensureProject(QStringLiteral("Somewhere Ferns"), 47217, 9999,
                               QStringLiteral("inat"));
    QVERIFY(!m_store->projectLocalityBox(noBboxProject).has_value());

    // A project whose place_inat_id was never actually upserted -> nullopt
    // (mirrors projectMembershipAndTree()'s own fixture, which never calls
    // upsertPlace() for its place id).
    const int uncachedPlaceProject =
        m_store->ensureProject(QStringLiteral("Diuris"), 60815, 424242,
                               QStringLiteral("inat"));
    QVERIFY(!m_store->projectLocalityBox(uncachedPlaceProject).has_value());

    // A project with no locality at all -> nullopt.
    const int noPlaceProject =
        m_store->ensureProject(QStringLiteral("No Locality"), 47217, std::nullopt,
                               QStringLiteral("inat"));
    QVERIFY(!m_store->projectLocalityBox(noPlaceProject).has_value());
}

void TestTaxonomyStore::taxonPhotoPersistsAndSurvivesPhotolessUpsert()
{
    Taxon t;
    t.inatId = 500;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Diuris pardina");
    t.photoUrl = QStringLiteral("https://inat.example/photos/9/medium.jpg");
    t.photoAttribution = QStringLiteral("(c) obs, CC BY-NC");
    QVERIFY(m_store->upsertTaxon(t) > 0);

    auto photoUrlOf = [this](qint64 inatId) {
        QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
        q.prepare(QStringLiteral("SELECT photo_url FROM taxon WHERE inat_id = ?"));
        q.addBindValue(inatId);
        return (q.exec() && q.next()) ? q.value(0).toString() : QString();
    };
    QCOMPARE(photoUrlOf(500), t.photoUrl);

    // The by-inat-id getter surfaces the same data (and reports absence).
    const auto tp = m_store->taxonPhoto(500);
    QVERIFY(tp.found);
    QCOMPARE(tp.name, QStringLiteral("Diuris pardina"));
    QCOMPARE(tp.photoUrl, t.photoUrl);
    QCOMPARE(tp.attribution, t.photoAttribution);
    QVERIFY(!m_store->taxonPhoto(999999).found);

    // A later upsert from an endpoint that didn't ask for default_photo must
    // not wipe the cached photo.
    Taxon photoless;
    photoless.inatId = 500;
    photoless.rank = QStringLiteral("species");
    photoless.name = QStringLiteral("Diuris pardina");
    QVERIFY(m_store->upsertTaxon(photoless) > 0);
    QCOMPARE(photoUrlOf(500), t.photoUrl);

    // A new non-empty value does win.
    Taxon updated = photoless;
    updated.photoUrl = QStringLiteral("https://inat.example/photos/10/medium.jpg");
    QVERIFY(m_store->upsertTaxon(updated) > 0);
    QCOMPARE(photoUrlOf(500), updated.photoUrl);
}

void TestTaxonomyStore::projectLeafPhotosListAndMissing()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.rankLevel = 20;
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    Taxon withPhoto;
    withPhoto.inatId = 900;
    withPhoto.parentInatId = 60815;
    withPhoto.rank = QStringLiteral("species");
    withPhoto.rankLevel = 10;
    withPhoto.name = QStringLiteral("Diuris pardina");
    withPhoto.commonName = QStringLiteral("Leopard Doubletail");
    withPhoto.photoUrl = QStringLiteral("https://inat.example/photos/1/medium.jpg");
    m_store->upsertTaxon(withPhoto);

    Taxon noPhoto;
    noPhoto.inatId = 901;
    noPhoto.parentInatId = 60815;
    noPhoto.rank = QStringLiteral("species");
    noPhoto.rankLevel = 10;
    noPhoto.name = QStringLiteral("Diuris punctata");
    m_store->upsertTaxon(noPhoto);

    const int proj = m_store->ensureProject(QStringLiteral("Diuris"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 900, true, false));
    QVERIFY(m_store->addProjectTaxon(proj, 901, true, false));

    const auto leaves = m_store->projectLeafPhotos(proj);
    QCOMPARE(leaves.size(), 2);   // the genus is not a leaf
    QCOMPARE(leaves.at(0).name, QStringLiteral("Diuris pardina"));   // name-sorted
    QCOMPARE(leaves.at(0).commonName, QStringLiteral("Leopard Doubletail"));
    QCOMPARE(leaves.at(0).photoUrl, QStringLiteral("https://inat.example/photos/1/medium.jpg"));
    QVERIFY(leaves.at(1).photoUrl.isEmpty());

    const auto missing = m_store->projectLeafTaxaMissingPhoto(proj);
    QCOMPARE(missing, QList<qint64>{901});

    // Scoped to a taxon: only its own subtree.
    const auto scoped = m_store->projectLeafPhotos(proj, 900);
    QCOMPARE(scoped.size(), 1);
    QCOMPARE(scoped.at(0).inatId, qint64(900));
}

void TestTaxonomyStore::deleteProjectRemovesItAndItsMembership()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids of Victoria"), 60815,
                                            std::nullopt, QStringLiteral("inat"));
    QVERIFY(proj > 0);
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QCOMPARE(m_store->projectTaxonInatIds(proj).size(), 1);

    QVERIFY(m_store->deleteProject(proj));

    QVERIFY(!m_store->projectIdByName(QStringLiteral("Orchids of Victoria")).has_value());
    QVERIFY(m_store->projectTaxonInatIds(proj).isEmpty());   // project_taxon cascaded away
    QVERIFY(m_store->taxonLocalId(60815).has_value());       // shared taxon cache untouched
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

void TestTaxonomyStore::pgAddProjectTaxonMergesFlagsOnConflict()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed TaxonomyStore tests");

    // Regression test: addProjectTaxon()'s ON CONFLICT clause used to merge
    // in_region/from_checklist with SQLite's scalar max(a, b) -- valid SQLite,
    // but Postgres's max() is aggregate-only and has no such overload, so
    // every call on Postgres failed outright (see TaxonomyStore.cpp).
    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));
    QSqlDatabase conn = QSqlDatabase::database(db.connectionName(), false);
    conn.exec(QStringLiteral(
        "TRUNCATE taxon, project, project_taxon RESTART IDENTITY CASCADE"));

    TaxonomyStore store(db.connectionName());
    Taxon fam;
    fam.inatId = 47217;
    fam.rank = QStringLiteral("family");
    fam.name = QStringLiteral("Orchidaceae");
    QVERIFY(store.upsertTaxon(fam) > 0);

    const int proj = store.ensureProject(QStringLiteral("Orchids of Victoria"), 47217,
                                         std::nullopt, QStringLiteral("inat"));
    QVERIFY(proj > 0);

    // First call: not in-region, not from a checklist.
    QVERIFY2(store.addProjectTaxon(proj, 47217, false, false), qPrintable(conn.lastError().text()));
    // Second call for the SAME taxon: in-region this time -- the ON CONFLICT
    // path must fire and merge, not just silently keep the old row untouched.
    QVERIFY2(store.addProjectTaxon(proj, 47217, true, false), qPrintable(conn.lastError().text()));

    const auto tree = store.projectTree(proj);
    QCOMPARE(tree.size(), 1);
    QVERIFY(tree.first().inRegion);
}

QTEST_GUILESS_MAIN(TestTaxonomyStore)
#include "tst_taxonomystore.moc"
