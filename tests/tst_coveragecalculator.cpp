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
