#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "coverage/CoverageCalculator.h"
#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::coverage;

class TestCoverageCalculator : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void rollsSpeciesCountsUpTheTree();
    void countsThreatenedSeparately();
    void countsOnlyStatusesForTheTreesRegion();
    void countsIntroducedSpeciesForTheTreesRegion();
    void picksTheMostSpecificAndMostAuthoritativeRecord();
    void rollsUpSubtreeStatusForFiltering();
    void infraspeciesPhotosCountForTheirSpecies();
    void onlyAutoAndConfirmedMatchesCount();
    void picksHighestConfidenceRepresentative();
    void representativeRollsUpToAncestors();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<taxonomy::TaxonomyStore> m_store;
    int m_projectId = -1;

    void addTaxon(qint64 id, qint64 parent, const QString &rank, const QString &name,
                  const QString &status = {});
    int captureInFolder(const QString &folderPath, const QString &baseName);
    void matchCapture(int captureId, qint64 taxonInatId, const QString &status);
};

void TestCoverageCalculator::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<taxonomy::TaxonomyStore>(m_db->connectionName());
    m_projectId = m_store->ensureProject(QStringLiteral("P"), 1, std::nullopt,
                                         QStringLiteral("inat"));
}

void TestCoverageCalculator::cleanup()
{
    m_store.reset();
    m_db.reset();
}

void TestCoverageCalculator::addTaxon(qint64 id, qint64 parent, const QString &rank,
                                      const QString &name, const QString &status)
{
    taxonomy::Taxon t;
    t.inatId = id;
    if (parent > 0)
        t.parentInatId = parent;
    t.rank = rank;
    t.name = name;
    m_store->upsertTaxon(t);
    m_store->addProjectTaxon(m_projectId, id, true, false);
    if (!status.isEmpty()) {
        taxonomy::StatusRecord s;
        s.status = status;
        s.source = QStringLiteral("VBA");
        m_store->addStatus(id, s);
    }
}

int TestCoverageCalculator::captureInFolder(const QString &folderPath, const QString &baseName)
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery f(db);
    f.prepare(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES (?, ?, 0)"));
    f.addBindValue(folderPath);
    f.addBindValue(folderPath);
    f.exec();
    const int folderId = f.lastInsertId().toInt();

    QSqlQuery c(db);
    c.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name, captured_on) VALUES (?, ?, ?)"));
    c.addBindValue(folderId);
    c.addBindValue(baseName);
    c.addBindValue(QStringLiteral("2024-10-01"));
    c.exec();
    return c.lastInsertId().toInt();
}

void TestCoverageCalculator::matchCapture(int captureId, qint64 taxonInatId, const QString &status)
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
        "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'file', 1.0, ?)"));
    q.addBindValue(captureId);
    q.addBindValue(taxonInatId);
    q.addBindValue(status);
    QVERIFY(q.exec());
}

void TestCoverageCalculator::rollsSpeciesCountsUpTheTree()
{
    addTaxon(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    addTaxon(10, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));
    addTaxon(11, 10, QStringLiteral("species"), QStringLiteral("Diuris pardina"));
    addTaxon(12, 10, QStringLiteral("species"), QStringLiteral("Diuris sulphurea"));
    addTaxon(13, 10, QStringLiteral("species"), QStringLiteral("Diuris orientis"));
    addTaxon(20, 1, QStringLiteral("genus"), QStringLiteral("Thelymitra"));
    addTaxon(21, 20, QStringLiteral("species"), QStringLiteral("Thelymitra ixioides"));

    matchCapture(captureInFolder(QStringLiteral("/a"), QStringLiteral("x")), 11,
                 QStringLiteral("auto"));
    matchCapture(captureInFolder(QStringLiteral("/b"), QStringLiteral("y")), 21,
                 QStringLiteral("confirmed"));

    const ProjectCoverage cov = computeCoverage(m_db->connectionName(), m_projectId);

    QCOMPARE(cov.speciesTotal, 4);
    QCOMPARE(cov.speciesWithPhotos, 2);
    QCOMPARE(cov.captureCount, 2);
    QCOMPARE(cov.newestCapture, QStringLiteral("2024-10-01"));

    QCOMPARE(cov.byTaxon.value(10).speciesTotal, 3);        // Diuris
    QCOMPARE(cov.byTaxon.value(10).speciesWithPhotos, 1);
    QCOMPARE(cov.byTaxon.value(1).speciesTotal, 4);         // family rolls up both genera
    QVERIFY(cov.byTaxon.value(11).hasOwnPhotos);
    QVERIFY(!cov.byTaxon.value(12).subtreeHasPhotos);
}

void TestCoverageCalculator::countsOnlyStatusesForTheTreesRegion()
{
    auto place = [&](qint64 id, QList<qint64> ancestors) {
        taxonomy::Place p;
        p.inatId = id;
        p.name = QString::number(id);
        p.ancestorIds = ancestors;
        QVERIFY(m_store->upsertPlace(p));
    };
    place(7830, {97393, 6744, 7830});            // Victoria
    place(52324, {97393, 6744, 7830, 52324});    // Grampians, within Victoria
    place(49109, {97393, 6744, 6899, 49109});    // The Coorong, SA
    place(6744, {97393, 6744});                  // Australia

    // Species 11 is listed for Victoria; species 12 has a status recorded
    // without a region.
    addTaxon(10, 0, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    addTaxon(11, 10, QStringLiteral("species"), QStringLiteral("Caladenia amoena"));
    addTaxon(12, 10, QStringLiteral("species"), QStringLiteral("Caladenia fulva"),
             QStringLiteral("Rare"));
    taxonomy::StatusRecord vic;
    vic.status = QStringLiteral("Endangered");
    vic.source = QStringLiteral("VBA 2021");
    vic.placeInatId = 7830;
    QVERIFY(m_store->addStatus(11, vic));

    auto threatenedIn = [&](std::optional<qint64> placeId, const QString &name) {
        const int pid = m_store->ensureProject(name, 10, placeId, QStringLiteral("inat"));
        for (qint64 id : {qint64(10), qint64(11), qint64(12)})
            m_store->addProjectTaxon(pid, id, true, false);
        const auto cov = coverage::computeCoverage(m_db->connectionName(), pid);
        return std::pair{cov.threatenedTotal, cov.byTaxon.value(11).status};
    };

    auto [victoria, vicStatus] = threatenedIn(7830, QStringLiteral("Victoria tree"));
    QCOMPARE(victoria, 2);
    QCOMPARE(vicStatus, QStringLiteral("Endangered"));
    QCOMPARE(threatenedIn(52324, QStringLiteral("Grampians tree")).first, 2);
    auto [coorong, coorongStatus] = threatenedIn(49109, QStringLiteral("Coorong tree"));
    QCOMPARE(coorong, 1);   // only the region-less status
    QVERIFY(coorongStatus.isEmpty());
    QCOMPARE(threatenedIn(6744, QStringLiteral("Australia tree")).first, 1);
    QCOMPARE(threatenedIn(std::nullopt, QStringLiteral("Worldwide tree")).first, 1);
}

void TestCoverageCalculator::countsIntroducedSpeciesForTheTreesRegion()
{
    auto place = [&](qint64 id, QList<qint64> ancestors) {
        taxonomy::Place p;
        p.inatId = id;
        p.name = QString::number(id);
        p.ancestorIds = ancestors;
        QVERIFY(m_store->upsertPlace(p));
    };
    place(7830, {97393, 6744, 7830});            // Victoria
    place(52324, {97393, 6744, 7830, 52324});    // Grampians, within Victoria
    place(49109, {97393, 6744, 6899, 49109});    // The Coorong, SA

    addTaxon(10, 0, QStringLiteral("genus"), QStringLiteral("Acacia"));
    addTaxon(11, 10, QStringLiteral("species"), QStringLiteral("Acacia baileyana"));
    addTaxon(12, 10, QStringLiteral("species"), QStringLiteral("Acacia longifolia"));
    addTaxon(13, 10, QStringLiteral("species"), QStringLiteral("Acacia pycnantha"));

    // 11: introduced per the VBA checklist. 12: introduced per iNaturalist,
    // but the checklist says native -- the checklist wins. 13: introduced
    // per iNaturalist only.
    auto origin = [&](qint64 id, const QString &value, const QString &source) {
        taxonomy::OriginRecord o;
        o.placeInatId = 7830;
        o.origin = value;
        o.source = source;
        QVERIFY(m_store->addOrigin(id, o));
    };
    origin(12, QStringLiteral("introduced"), QStringLiteral("iNaturalist"));
    origin(11, QStringLiteral("introduced"), QStringLiteral("VBA 2021"));
    origin(12, QStringLiteral("native"), QStringLiteral("VBA 2021"));
    QCOMPARE(m_store->addIntroduced(7830, QStringLiteral("iNaturalist"), {13, 999999}), 1);

    // 11 is photographed.
    matchCapture(captureInFolder(QStringLiteral("/lib/Acacia"), QStringLiteral("baileyana")), 11,
                 QStringLiteral("confirmed"));

    auto coverageIn = [&](qint64 placeId, const QString &name) {
        const int pid = m_store->ensureProject(name, 10, placeId, QStringLiteral("inat"));
        for (qint64 id : {qint64(10), qint64(11), qint64(12), qint64(13)})
            m_store->addProjectTaxon(pid, id, true, false);
        return coverage::computeCoverage(m_db->connectionName(), pid);
    };

    const auto grampians = coverageIn(52324, QStringLiteral("Grampians tree"));
    QCOMPARE(grampians.introducedTotal, 2);   // 11 and 13, not 12
    QCOMPARE(grampians.introducedWithPhotos, 1);
    QVERIFY(grampians.byTaxon.value(11).introduced);
    QVERIFY(!grampians.byTaxon.value(12).introduced);
    QCOMPARE(grampians.byTaxon.value(10).introducedTotal, 2);   // rolled up to the genus

    const auto coorong = coverageIn(49109, QStringLiteral("Coorong tree"));
    QCOMPARE(coorong.introducedTotal, 0);   // Victorian records don't apply in SA
}

void TestCoverageCalculator::picksTheMostSpecificAndMostAuthoritativeRecord()
{
    auto place = [&](qint64 id, QList<qint64> ancestors) {
        taxonomy::Place p;
        p.inatId = id;
        p.name = QString::number(id);
        p.ancestorIds = ancestors;
        QVERIFY(m_store->upsertPlace(p));
    };
    // Tree regions only: the state places the records point at (7830 aside)
    // aren't cached, as for most states in practice.
    place(52324, {97393, 6744, 7830, 52324});   // Grampians
    place(7830, {97393, 6744, 7830});           // Victoria
    place(99001, {97393, 6744, 6825, 99001});   // somewhere in NSW
    place(6744, {97393, 6744});                 // Australia

    addTaxon(20, 0, QStringLiteral("genus"), QStringLiteral("G"));
    for (qint64 id = 21; id <= 26; ++id)
        addTaxon(id, 20, QStringLiteral("species"), QStringLiteral("G s%1").arg(id));

    auto status = [&](qint64 id, qint64 placeId, const QString &value, const QString &source) {
        taxonomy::StatusRecord s;
        s.placeInatId = placeId;
        s.status = value;
        s.source = source;
        QVERIFY(m_store->addStatus(id, s));
    };
    status(21, 6744, QStringLiteral("Endangered"), QStringLiteral("ALA dr656"));   // EPBC
    status(21, 7830, QStringLiteral("Critically Endangered"), QStringLiteral("ALA dr655"));
    status(22, 7830, QStringLiteral("Rare"), QStringLiteral("VBA 2021"));
    status(22, 7830, QStringLiteral("Vulnerable"), QStringLiteral("ALA dr655"));
    status(23, 7830, QStringLiteral("Poorly known"), QStringLiteral("VBA 2021"));
    status(24, 6825, QStringLiteral("Endangered"), QStringLiteral("ALA dr650"));   // NSW

    auto origin = [&](qint64 id, qint64 placeId, const QString &value, const QString &source) {
        taxonomy::OriginRecord o;
        o.placeInatId = placeId;
        o.origin = value;
        o.source = source;
        QVERIFY(m_store->addOrigin(id, o));
    };
    origin(25, 7830, QStringLiteral("introduced"), QStringLiteral("iNaturalist"));
    origin(25, 7830, QStringLiteral("native"), QStringLiteral("APC"));
    origin(26, 52324, QStringLiteral("introduced"), QStringLiteral("iNaturalist"));
    origin(26, 7830, QStringLiteral("native"), QStringLiteral("APC"));

    auto coverageIn = [&](qint64 placeId, const QString &name) {
        const int pid = m_store->ensureProject(name, 20, placeId, QStringLiteral("inat"));
        for (qint64 id = 20; id <= 26; ++id)
            m_store->addProjectTaxon(pid, id, true, false);
        return coverage::computeCoverage(m_db->connectionName(), pid);
    };

    const auto grampians = coverageIn(52324, QStringLiteral("Grampians"));
    QCOMPARE(grampians.byTaxon.value(21).status, QStringLiteral("Critically Endangered"));
    QCOMPARE(grampians.byTaxon.value(22).status, QStringLiteral("Vulnerable"));   // ALA over VBA
    QCOMPARE(grampians.byTaxon.value(23).status, QStringLiteral("Poorly known"));   // VBA only
    QVERIFY(grampians.byTaxon.value(24).status.isEmpty());   // NSW listing
    QVERIFY(!grampians.byTaxon.value(25).introduced);        // APC over iNaturalist
    QVERIFY(!grampians.byTaxon.value(26).introduced);        // even iNat's narrower region
    QCOMPARE(grampians.introducedTotal, 0);

    const auto australia = coverageIn(6744, QStringLiteral("Australia"));
    QCOMPARE(australia.byTaxon.value(21).status, QStringLiteral("Endangered"));   // EPBC only
    QVERIFY(australia.byTaxon.value(22).status.isEmpty());
    QCOMPARE(australia.threatenedTotal, 1);

    const auto nsw = coverageIn(99001, QStringLiteral("NSW place"));
    QCOMPARE(nsw.byTaxon.value(24).status, QStringLiteral("Endangered"));   // state not cached
    QCOMPARE(nsw.byTaxon.value(21).status, QStringLiteral("Endangered"));   // EPBC
}

void TestCoverageCalculator::countsThreatenedSeparately()
{
    addTaxon(1, 0, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    addTaxon(2, 1, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    addTaxon(3, 1, QStringLiteral("species"), QStringLiteral("Caladenia rosella"),
             QStringLiteral("Endangered"));
    addTaxon(4, 1, QStringLiteral("species"), QStringLiteral("Caladenia concolor"),
             QStringLiteral("Endangered"));
    addTaxon(5, 1, QStringLiteral("species"), QStringLiteral("Caladenia venusta"),
             QStringLiteral("Vulnerable"));

    matchCapture(captureInFolder(QStringLiteral("/a"), QStringLiteral("x")), 3,
                 QStringLiteral("auto"));

    const ProjectCoverage cov = computeCoverage(m_db->connectionName(), m_projectId);

    QCOMPARE(cov.threatenedTotal, 3);
    QCOMPARE(cov.threatenedWithPhotos, 1);
    QCOMPARE(cov.byStatus.value(QStringLiteral("Endangered")).total, 2);
    QCOMPARE(cov.byStatus.value(QStringLiteral("Endangered")).withPhotos, 1);
    QCOMPARE(cov.byStatus.value(QStringLiteral("Vulnerable")).total, 1);
    QCOMPARE(cov.byStatus.value(QStringLiteral("Vulnerable")).withPhotos, 0);
}

void TestCoverageCalculator::rollsUpSubtreeStatusForFiltering()
{
    addTaxon(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    addTaxon(10, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    addTaxon(11, 10, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    addTaxon(12, 10, QStringLiteral("species"), QStringLiteral("Caladenia rosella"),
             QStringLiteral("Endangered"));
    addTaxon(20, 1, QStringLiteral("genus"), QStringLiteral("Thelymitra"));
    addTaxon(21, 20, QStringLiteral("species"), QStringLiteral("Thelymitra ixioides"));
    // An infraspecific taxon can carry its own status without being tallied
    // into byStatus/threatenedTotal (that rollup is species-only).
    addTaxon(22, 21, QStringLiteral("variety"), QStringLiteral("Thelymitra ixioides var. x"),
             QStringLiteral("Vulnerable"));

    const ProjectCoverage cov = computeCoverage(m_db->connectionName(), m_projectId);

    // The genus and family ancestors of the Endangered species see it in
    // their subtree; the sibling genus with no threatened species does not.
    QVERIFY(cov.byTaxon.value(10).subtreeThreatened);
    QVERIFY(cov.byTaxon.value(1).subtreeThreatened);
    QVERIFY(cov.byTaxon.value(10).subtreeStatuses.contains(QStringLiteral("Endangered")));
    QVERIFY(cov.byTaxon.value(1).subtreeStatuses.contains(QStringLiteral("Endangered")));

    // The variety's own status is recorded...
    QCOMPARE(cov.byTaxon.value(22).status, QStringLiteral("Vulnerable"));
    // ...but since it isn't a species, it doesn't count toward the species
    // tally, and its ancestors' subtree rollup stays clear of it.
    QCOMPARE(cov.threatenedTotal, 1);
    QVERIFY(!cov.byStatus.contains(QStringLiteral("Vulnerable")));
    QVERIFY(!cov.byTaxon.value(20).subtreeThreatened);
    QVERIFY(!cov.byTaxon.value(21).subtreeThreatened);
}

void TestCoverageCalculator::infraspeciesPhotosCountForTheirSpecies()
{
    addTaxon(1, 0, QStringLiteral("genus"), QStringLiteral("Olearia"));
    addTaxon(2, 1, QStringLiteral("species"), QStringLiteral("Olearia ramulosa"));
    addTaxon(3, 2, QStringLiteral("variety"), QStringLiteral("Olearia ramulosa var. stricta"));

    matchCapture(captureInFolder(QStringLiteral("/a"), QStringLiteral("x")), 3,
                 QStringLiteral("auto"));

    const ProjectCoverage cov = computeCoverage(m_db->connectionName(), m_projectId);

    QCOMPARE(cov.speciesTotal, 1);          // one species unit
    QCOMPARE(cov.speciesWithPhotos, 1);     // its variety's photo counts
    QVERIFY(cov.byTaxon.value(2).subtreeHasPhotos);
}

void TestCoverageCalculator::onlyAutoAndConfirmedMatchesCount()
{
    addTaxon(1, 0, QStringLiteral("genus"), QStringLiteral("Diuris"));
    addTaxon(2, 1, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    matchCapture(captureInFolder(QStringLiteral("/a"), QStringLiteral("x")), 2,
                 QStringLiteral("pending"));
    matchCapture(captureInFolder(QStringLiteral("/b"), QStringLiteral("y")), 2,
                 QStringLiteral("rejected"));

    const ProjectCoverage cov = computeCoverage(m_db->connectionName(), m_projectId);
    QCOMPARE(cov.speciesWithPhotos, 0);
    QCOMPARE(cov.captureCount, 0);
}

void TestCoverageCalculator::picksHighestConfidenceRepresentative()
{
    addTaxon(1, 0, QStringLiteral("genus"), QStringLiteral("Diuris"));
    addTaxon(2, 1, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    const int weak = captureInFolder(QStringLiteral("/a"), QStringLiteral("weak"));
    const int strong = captureInFolder(QStringLiteral("/b"), QStringLiteral("strong"));

    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery q(db);
    q.exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                          "VALUES (%1, (SELECT id FROM taxon WHERE inat_id=2), 'file', 0.7, 'auto')")
               .arg(weak));
    q.exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                          "VALUES (%1, (SELECT id FROM taxon WHERE inat_id=2), 'file', 1.0, 'auto')")
               .arg(strong));

    QCOMPARE(pickRepresentatives(m_db->connectionName(), m_projectId), 2);   // species + genus

    const auto rep = representativeFor(m_db->connectionName(), m_projectId, 2);
    QCOMPARE(rep.captureId, qint64(strong));
}

void TestCoverageCalculator::representativeRollsUpToAncestors()
{
    addTaxon(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    addTaxon(10, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));
    addTaxon(11, 10, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    const int cap = captureInFolder(QStringLiteral("/a"), QStringLiteral("x"));
    matchCapture(cap, 11, QStringLiteral("auto"));

    pickRepresentatives(m_db->connectionName(), m_projectId);

    QCOMPARE(representativeFor(m_db->connectionName(), m_projectId, 11).captureId, qint64(cap));
    QCOMPARE(representativeFor(m_db->connectionName(), m_projectId, 10).captureId, qint64(cap));
    QCOMPARE(representativeFor(m_db->connectionName(), m_projectId, 1).captureId, qint64(cap));
}

QTEST_GUILESS_MAIN(TestCoverageCalculator)
#include "tst_coveragecalculator.moc"
