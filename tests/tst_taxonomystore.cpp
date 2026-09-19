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
    void foldSearchTextNormalisesHybridMarker();
    void placeRoundTrips();
    void taxonUpsertStoresNamesAndIsIdempotent();
    void reUpsertReplacesNamesWithoutDuplicating();
    void projectMembershipAndTree();
    void projectLocalityBoxResolvesFromPlace();
    void taxonPhotoPersistsAndSurvivesPhotolessUpsert();
    void projectLeafPhotosListAndMissing();
    void deleteProjectRemovesItAndItsMembership();
    void pruneTaxonFromProjectRemovesSelfOnly();
    void pruneTaxonFromProjectCascadesToDescendants();
    void pruneTaxonFromProjectDoesNotAffectOtherProjects();
    void projectIsPrunedReflectsExclusions();
    void addTaxonWithAncestorsAddsWholeChain();
    void addTaxonWithAncestorsClearsPriorExclusion();
    void addTaxonWithAncestorsIsIdempotent();
    void upsertTaxaWritesAllRowsInOnePass();
    void upsertTaxaDedupesRepeatedInatId();
    void upsertTaxaChunksLargeBatches();
    void addProjectTaxaLinksAllAndMergesFlags();
    void httpCacheRoundTrips();
    void buildCheckpointRoundTripsUpsertsAndClears();
    void projectSpeciesAncestorIdsDerivedFromAncestryColumn();
    void pgAddProjectTaxonMergesFlagsOnConflict();
    void pgRollbackBatchRecoversConnectionAfterFailure();
    void pgSaveBuildCheckpointParticipatesInBatch();
    void pgUpsertTaxaDedupeAvoidsConflictError();

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

void TestTaxonomyStore::foldSearchTextNormalisesHybridMarker()
{
    // A user types a bare "x" for the hybrid marker; the stored taxon name
    // carries the multiplication sign, so a typed search needs to be folded
    // the same way to find it.
    QCOMPARE(TaxonomyStore::foldSearchText(QStringLiteral("Eucalyptus x carolaniae")),
             QString::fromUtf8("eucalyptus \xC3\x97 carolaniae"));
    QCOMPARE(TaxonomyStore::foldSearchText(QStringLiteral("Eucalyptus X carolaniae")),
             QString::fromUtf8("eucalyptus \xC3\x97 carolaniae"));
    // Already-typed multiplication sign passes through unchanged.
    QCOMPARE(TaxonomyStore::foldSearchText(QString::fromUtf8("Eucalyptus \xC3\x97 carolaniae")),
             QString::fromUtf8("eucalyptus \xC3\x97 carolaniae"));
    // "x" inside a word (not a standalone token) is never touched.
    QCOMPARE(TaxonomyStore::foldSearchText(QStringLiteral("Xanthorrhoea")),
             QStringLiteral("xanthorrhoea"));
    QCOMPARE(TaxonomyStore::foldSearchText(QStringLiteral("Banksia rex")),
             QStringLiteral("banksia rex"));
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

void TestTaxonomyStore::pruneTaxonFromProjectRemovesSelfOnly()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    Taxon sp;
    sp.inatId = 200000;
    sp.parentInatId = 60815;
    sp.rank = QStringLiteral("species");
    sp.name = QStringLiteral("Diuris pardina");
    m_store->upsertTaxon(sp);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200000, true, false));
    QCOMPARE(m_store->projectTaxonInatIds(proj).size(), 2);

    QCOMPARE(m_store->projectPruneCount(proj, 200000), 1);
    QCOMPARE(m_store->pruneTaxonFromProject(proj, 200000), 1);

    const auto remaining = m_store->projectTaxonInatIds(proj);
    QCOMPARE(remaining.size(), 1);
    QVERIFY(remaining.contains(60815));
    QVERIFY(m_store->taxonLocalId(200000).has_value());   // shared cache untouched
}

void TestTaxonomyStore::pruneTaxonFromProjectCascadesToDescendants()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    Taxon sp1;
    sp1.inatId = 200000;
    sp1.parentInatId = 60815;
    sp1.rank = QStringLiteral("species");
    sp1.name = QStringLiteral("Diuris pardina");
    m_store->upsertTaxon(sp1);

    Taxon sp2;
    sp2.inatId = 200001;
    sp2.parentInatId = 60815;
    sp2.rank = QStringLiteral("species");
    sp2.name = QStringLiteral("Diuris sulphurea");
    m_store->upsertTaxon(sp2);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200000, true, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200001, true, false));
    QCOMPARE(m_store->projectTaxonInatIds(proj).size(), 3);

    QCOMPARE(m_store->projectPruneCount(proj, 60815), 3);
    QCOMPARE(m_store->pruneTaxonFromProject(proj, 60815), 3);

    QVERIFY(m_store->projectTaxonInatIds(proj).isEmpty());
    QVERIFY(m_store->taxonLocalId(60815).has_value());     // shared cache untouched
    QVERIFY(m_store->taxonLocalId(200000).has_value());
    QVERIFY(m_store->taxonLocalId(200001).has_value());

    const auto excluded = m_store->projectExcludedTaxonIds(proj);
    QCOMPARE(excluded.size(), 3);
    QVERIFY(excluded.contains(60815));
    QVERIFY(excluded.contains(200000));
    QVERIFY(excluded.contains(200001));
}

void TestTaxonomyStore::pruneTaxonFromProjectDoesNotAffectOtherProjects()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    const int projA = m_store->ensureProject(QStringLiteral("Project A"), 60815, std::nullopt,
                                             QStringLiteral("inat"));
    const int projB = m_store->ensureProject(QStringLiteral("Project B"), 60815, std::nullopt,
                                             QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(projA, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(projB, 60815, false, false));

    QCOMPARE(m_store->pruneTaxonFromProject(projA, 60815), 1);

    QVERIFY(m_store->projectTaxonInatIds(projA).isEmpty());
    QCOMPARE(m_store->projectTaxonInatIds(projB).size(), 1);
    QVERIFY(!m_store->projectIsPruned(projB));
}

void TestTaxonomyStore::projectIsPrunedReflectsExclusions()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));

    QVERIFY(!m_store->projectIsPruned(proj));
    QVERIFY(m_store->pruneTaxonFromProject(proj, 60815) >= 0);
    QVERIFY(m_store->projectIsPruned(proj));
}

void TestTaxonomyStore::addTaxonWithAncestorsAddsWholeChain()
{
    // The project's own root, unrelated to the chain being added.
    Taxon root;
    root.inatId = 1;
    root.rank = QStringLiteral("kingdom");
    root.name = QStringLiteral("Animalia");
    m_store->upsertTaxon(root);
    const int proj = m_store->ensureProject(QStringLiteral("Animals"), 1, std::nullopt,
                                            QStringLiteral("inat"));

    Taxon kingdom;
    kingdom.inatId = 48460;
    kingdom.rank = QStringLiteral("kingdom");
    kingdom.name = QStringLiteral("Plantae");

    Taxon family;
    family.inatId = 47217;
    family.parentInatId = 48460;
    family.rank = QStringLiteral("family");
    family.name = QStringLiteral("Orchidaceae");

    Taxon species;
    species.inatId = 900;
    species.parentInatId = 47217;
    species.rank = QStringLiteral("species");
    species.name = QStringLiteral("Diuris pardina");

    QVERIFY(m_store->addTaxonWithAncestors(proj, {kingdom, family}, species));

    const auto ids = m_store->projectTaxonInatIds(proj);
    QVERIFY(ids.contains(48460));
    QVERIFY(ids.contains(47217));
    QVERIFY(ids.contains(900));

    const auto tree = m_store->projectTree(proj);
    auto nodeFor = [&](qint64 inatId) {
        return std::find_if(tree.begin(), tree.end(),
                            [&](const auto &n) { return n.inatId == inatId; });
    };
    QVERIFY(nodeFor(900) != tree.end());
    QVERIFY(nodeFor(900)->inRegion);        // a leaf-rank taxon
    QVERIFY(!nodeFor(48460)->inRegion);     // ancestors are never in-region
    QVERIFY(!nodeFor(47217)->inRegion);
    QCOMPARE(nodeFor(900)->parentInatId.value_or(-1), qint64(47217));
}

void TestTaxonomyStore::addTaxonWithAncestorsClearsPriorExclusion()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    Taxon sp;
    sp.inatId = 200000;
    sp.parentInatId = 60815;
    sp.rank = QStringLiteral("species");
    sp.name = QStringLiteral("Diuris pardina");
    m_store->upsertTaxon(sp);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 60815, false, false));
    QVERIFY(m_store->addProjectTaxon(proj, 200000, true, false));

    QCOMPARE(m_store->pruneTaxonFromProject(proj, 60815), 2);   // genus + species
    QVERIFY(m_store->projectTaxonInatIds(proj).isEmpty());
    auto excluded = m_store->projectExcludedTaxonIds(proj);
    QVERIFY(excluded.contains(60815));
    QVERIFY(excluded.contains(200000));

    // Explicitly re-adding the species (with the genus as its one ancestor)
    // must clear both exclusions, not just add the rows back.
    QVERIFY(m_store->addTaxonWithAncestors(proj, {genus}, sp));

    excluded = m_store->projectExcludedTaxonIds(proj);
    QVERIFY(!excluded.contains(60815));
    QVERIFY(!excluded.contains(200000));

    const auto ids = m_store->projectTaxonInatIds(proj);
    QVERIFY(ids.contains(60815));
    QVERIFY(ids.contains(200000));
}

void TestTaxonomyStore::addTaxonWithAncestorsIsIdempotent()
{
    Taxon family;
    family.inatId = 47217;
    family.rank = QStringLiteral("family");
    family.name = QStringLiteral("Orchidaceae");
    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 47217, std::nullopt,
                                            QStringLiteral("inat"));

    Taxon species;
    species.inatId = 900;
    species.parentInatId = 47217;
    species.rank = QStringLiteral("species");
    species.name = QStringLiteral("Diuris pardina");

    QVERIFY(m_store->addTaxonWithAncestors(proj, {family}, species));
    QVERIFY(m_store->addTaxonWithAncestors(proj, {family}, species));

    QCOMPARE(m_store->projectTaxonInatIds(proj).size(), 2);   // no duplicates
    QCOMPARE(m_store->taxonCount(), 2);
}

void TestTaxonomyStore::upsertTaxaWritesAllRowsInOnePass()
{
    Taxon a;
    a.inatId = 900;
    a.rank = QStringLiteral("species");
    a.name = QStringLiteral("Diuris pardina");
    a.commonName = QStringLiteral("Leopard Doubletail");
    a.synonyms = {QStringLiteral("Diuris punctata var. pardina")};
    a.vernacular = {QStringLiteral("Leopard Doubletail"), QStringLiteral("Spotted Doubletail")};

    Taxon b;
    b.inatId = 901;
    b.rank = QStringLiteral("species");
    b.name = QStringLiteral("Caladenia carnea");

    const auto ids = m_store->upsertTaxa({a, b});
    QCOMPARE(ids.size(), 2);
    QVERIFY(ids.contains(900));
    QVERIFY(ids.contains(901));
    QCOMPARE(m_store->taxonCount(), 2);
    QCOMPARE(m_store->taxonLocalId(900).value_or(-1), qint64(ids.value(900)));

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM taxon_name WHERE taxon_id = ?"));
    q.addBindValue(ids.value(900));
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 4);   // accepted + 1 synonym + 2 vernacular

    // Re-upserting with a smaller name set replaces, doesn't accumulate.
    a.synonyms.clear();
    a.vernacular.clear();
    QVERIFY(!m_store->upsertTaxa({a}).isEmpty());
    q.addBindValue(ids.value(900));
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 1);   // just "accepted" now
}

void TestTaxonomyStore::upsertTaxaDedupesRepeatedInatId()
{
    Taxon first;
    first.inatId = 900;
    first.rank = QStringLiteral("species");
    first.name = QStringLiteral("Diuris pardina");

    Taxon second = first;
    second.name = QStringLiteral("Diuris pardina (updated)");

    // Two entries sharing the same inat_id in one call -- must not error
    // (a naive multi-row Postgres UPSERT would reject this), and the last
    // one wins.
    const auto ids = m_store->upsertTaxa({first, second});
    QCOMPARE(ids.size(), 1);
    QCOMPARE(m_store->taxonCount(), 1);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT name FROM taxon WHERE inat_id = 900"));
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toString(), second.name);
}

void TestTaxonomyStore::upsertTaxaChunksLargeBatches()
{
    QList<Taxon> taxa;
    constexpr int kCount = 250;   // bigger than one chunk at the current budget
    for (int i = 0; i < kCount; ++i) {
        Taxon t;
        t.inatId = 100000 + i;
        t.rank = QStringLiteral("species");
        t.name = QStringLiteral("Testus sp%1").arg(i);
        taxa.append(t);
    }

    const auto ids = m_store->upsertTaxa(taxa);
    QCOMPARE(ids.size(), kCount);
    QCOMPARE(m_store->taxonCount(), kCount);
    for (int i = 0; i < kCount; ++i)
        QVERIFY(ids.contains(100000 + i));
}

void TestTaxonomyStore::addProjectTaxaLinksAllAndMergesFlags()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");

    Taxon species;
    species.inatId = 900;
    species.parentInatId = 60815;
    species.rank = QStringLiteral("species");
    species.name = QStringLiteral("Diuris pardina");

    const auto ids = m_store->upsertTaxa({genus, species});
    const int proj = m_store->ensureProject(QStringLiteral("Diuris"), 60815, std::nullopt,
                                            QStringLiteral("inat"));

    // Two entries for the same taxon in one call, with different flags --
    // must merge (OR), matching what the DB's own ON CONFLICT does, not
    // just take one of them.
    QVERIFY(m_store->addProjectTaxa(
        proj, {{ids.value(60815), false, false},
               {ids.value(900), true, false},
               {ids.value(900), false, true}}));

    const auto tree = m_store->projectTree(proj);
    auto nodeFor = [&](qint64 inatId) {
        return std::find_if(tree.begin(), tree.end(),
                            [&](const auto &n) { return n.inatId == inatId; });
    };
    QCOMPARE(tree.size(), 2);
    QVERIFY(!nodeFor(60815)->inRegion);
    QVERIFY(nodeFor(900)->inRegion);
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

void TestTaxonomyStore::buildCheckpointRoundTripsUpsertsAndClears()
{
    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    m_store->upsertTaxon(genus);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 60815, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(!m_store->buildCheckpoint(proj).has_value());

    BuildCheckpoint c;
    c.rootTaxonInatId = 60815;
    c.placeInatId = 6744;
    c.perPage = 200;
    c.nextPage = 3;
    c.speciesSeen = 400;
    c.speciesTotal = 900;
    QVERIFY(m_store->saveBuildCheckpoint(proj, c));

    auto got = m_store->buildCheckpoint(proj);
    QVERIFY(got.has_value());
    QCOMPARE(got->rootTaxonInatId, qint64(60815));
    QCOMPARE(got->placeInatId.value_or(-1), qint64(6744));
    QCOMPARE(got->perPage, 200);
    QCOMPARE(got->nextPage, 3);
    QCOMPARE(got->speciesSeen, 400);
    QCOMPARE(got->speciesTotal, 900);

    // Saving again (as a later page of the same run would) upserts in place,
    // one row per project -- not a growing history.
    c.nextPage = 4;
    c.speciesSeen = 600;
    QVERIFY(m_store->saveBuildCheckpoint(proj, c));
    QCOMPARE(m_store->buildCheckpoint(proj)->nextPage, 4);
    QCOMPARE(m_store->buildCheckpoint(proj)->speciesSeen, 600);

    QVERIFY(m_store->clearBuildCheckpoint(proj));
    QVERIFY(!m_store->buildCheckpoint(proj).has_value());
}

void TestTaxonomyStore::projectSpeciesAncestorIdsDerivedFromAncestryColumn()
{
    Taxon fam;
    fam.inatId = 47217;
    fam.rank = QStringLiteral("family");
    fam.name = QStringLiteral("Orchidaceae");
    m_store->upsertTaxon(fam);

    Taxon sp1;
    sp1.inatId = 900;
    sp1.parentInatId = 47217;
    sp1.rank = QStringLiteral("species");
    sp1.name = QStringLiteral("Diuris pardina");
    sp1.ancestry = QStringLiteral("48460/47126/47217/800");
    m_store->upsertTaxon(sp1);

    Taxon sp2;
    sp2.inatId = 901;
    sp2.parentInatId = 47217;
    sp2.rank = QStringLiteral("species");
    sp2.name = QStringLiteral("Caladenia carnea");
    sp2.ancestry = QStringLiteral("48460/47126/47217/801");
    m_store->upsertTaxon(sp2);

    const int proj = m_store->ensureProject(QStringLiteral("Orchids"), 47217, std::nullopt,
                                            QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(proj, 47217, false, false));   // not in_region: excluded
    QVERIFY(m_store->addProjectTaxon(proj, 900, true, false));
    QVERIFY(m_store->addProjectTaxon(proj, 901, true, false));

    const auto ids = m_store->projectSpeciesAncestorIds(proj);
    QCOMPARE(ids.size(), 5);
    for (qint64 id : {qint64(48460), qint64(47126), qint64(47217), qint64(800), qint64(801)})
        QVERIFY(ids.contains(id));
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

void TestTaxonomyStore::pgRollbackBatchRecoversConnectionAfterFailure()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed TaxonomyStore tests");

    // Regression test: on Postgres, any failed statement inside a
    // begin/commit batch marks the whole transaction "aborted" -- every
    // later statement on that connection then fails until an explicit
    // ROLLBACK. ProjectBuilder used to never call rollbackBatch() on a
    // failed batch, so one bad write during a tree build could silently
    // poison the shared connection for the rest of the session (surfacing
    // later as "can't refresh"/"can't delete" on an unrelated reference
    // tree). This confirms rollbackBatch() actually restores the connection.
    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));
    QSqlDatabase conn = QSqlDatabase::database(db.connectionName(), false);
    conn.exec(QStringLiteral("TRUNCATE taxon, project, project_taxon RESTART IDENTITY CASCADE"));

    TaxonomyStore store(db.connectionName());
    QVERIFY(store.beginBatch());

    QSqlQuery bad(conn);
    QVERIFY(!bad.exec(QStringLiteral("SELECT 1/0")));

    store.rollbackBatch();

    Taxon t;
    t.inatId = 999999;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Testus regressus");
    QVERIFY2(store.upsertTaxon(t) > 0, qPrintable(conn.lastError().text()));
}

void TestTaxonomyStore::pgSaveBuildCheckpointParticipatesInBatch()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed TaxonomyStore tests");

    // ProjectBuilder::fetchSpeciesPage() saves the checkpoint inside the same
    // beginBatch()/commitBatch() transaction as the page's own writes, so a
    // rollback must undo both together, and a commit must persist both.
    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));
    QSqlDatabase conn = QSqlDatabase::database(db.connectionName(), false);
    conn.exec(QStringLiteral(
        "TRUNCATE taxon, project, project_taxon, project_build_checkpoint "
        "RESTART IDENTITY CASCADE"));

    TaxonomyStore store(db.connectionName());
    Taxon fam;
    fam.inatId = 47217;
    fam.rank = QStringLiteral("family");
    fam.name = QStringLiteral("Orchidaceae");
    QVERIFY(store.upsertTaxon(fam) > 0);
    const int proj = store.ensureProject(QStringLiteral("Orchids"), 47217, std::nullopt,
                                         QStringLiteral("inat"));
    QVERIFY(proj > 0);

    BuildCheckpoint c;
    c.rootTaxonInatId = 47217;
    c.perPage = 200;
    c.nextPage = 2;
    c.speciesSeen = 100;
    c.speciesTotal = 500;

    QVERIFY(store.beginBatch());
    QVERIFY2(store.saveBuildCheckpoint(proj, c), qPrintable(conn.lastError().text()));
    store.rollbackBatch();
    QVERIFY(!store.buildCheckpoint(proj).has_value());   // rolled back with the rest of the batch

    QVERIFY(store.beginBatch());
    QVERIFY2(store.saveBuildCheckpoint(proj, c), qPrintable(conn.lastError().text()));
    QVERIFY(store.commitBatch());
    const auto got = store.buildCheckpoint(proj);
    QVERIFY(got.has_value());
    QCOMPARE(got->nextPage, 2);
}

void TestTaxonomyStore::pgUpsertTaxaDedupeAvoidsConflictError()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed TaxonomyStore tests");

    // Regression test: Postgres's ON CONFLICT ... DO UPDATE raises "cannot
    // affect row a second time" if one multi-row INSERT targets the same
    // conflicting key (inat_id) twice -- SQLite silently tolerates this, so
    // only a real Postgres run catches a missing/broken dedup step.
    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));
    QSqlDatabase conn = QSqlDatabase::database(db.connectionName(), false);
    conn.exec(QStringLiteral("TRUNCATE taxon, taxon_name RESTART IDENTITY CASCADE"));

    TaxonomyStore store(db.connectionName());
    Taxon first;
    first.inatId = 900;
    first.rank = QStringLiteral("species");
    first.name = QStringLiteral("Diuris pardina");
    Taxon second = first;
    second.name = QStringLiteral("Diuris pardina (updated)");

    const auto ids = store.upsertTaxa({first, second});
    QVERIFY2(!ids.isEmpty(), qPrintable(conn.lastError().text()));
    QCOMPARE(ids.size(), 1);

    QSqlQuery q(conn);
    q.prepare(QStringLiteral("SELECT name FROM taxon WHERE inat_id = 900"));
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toString(), second.name);
}

QTEST_GUILESS_MAIN(TestTaxonomyStore)
#include "tst_taxonomystore.moc"
