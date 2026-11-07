#include <QtTest>

#include "coverage/SeenTally.h"

using namespace pl;
using namespace pl::coverage;

namespace {

taxonomy::TreeNode node(qint64 id, qint64 parent, const char *rank)
{
    taxonomy::TreeNode n;
    n.inatId = id;
    if (parent > 0)
        n.parentInatId = parent;
    n.rank = QString::fromLatin1(rank);
    n.name = QStringLiteral("t%1").arg(id);
    n.isLeafRank = QByteArray(rank) == "species" || QByteArray(rank) == "subspecies"
                   || QByteArray(rank) == "variety" || QByteArray(rank) == "genushybrid";
    return n;
}

// Parents first, as TaxonomyStore::projectTree gives them.
QList<taxonomy::TreeNode> tree()
{
    return {
        node(1, 0, "family"),
        node(10, 1, "genus"),       node(20, 1, "genus"),
        node(100, 10, "species"),   node(101, 10, "species"), node(200, 20, "species"),
        node(30, 1, "genushybrid"),
        node(1010, 101, "subspecies"),
        node(10100, 1010, "variety"),
    };
}

ProjectCoverage photographed(const QList<qint64> &ids)
{
    ProjectCoverage cov;
    for (qint64 id : ids)
        cov.byTaxon[id].subtreeHasPhotos = true;
    return cov;
}

} // namespace

class TestSeenTally : public QObject
{
    Q_OBJECT

private slots:
    void photographedIsSeen();
    void markedLowerTaxonMakesItsSpeciesSeen();
    void speciesSeenRollsUp();
    void hybridsAreNotSpecies();
    void talliesOnlyWhatIsShown();
};

void TestSeenTally::photographedIsSeen()
{
    const auto s = rollUpSeen(tree(), photographed({1, 10, 100}), {});
    QVERIFY(s[100].seen);
    QVERIFY(s[100].photographed);
    QVERIFY(!s[100].marked);
    QVERIFY(!s[101].seen);
    QVERIFY(!s[10].seen);   // a genus is never seen itself
}

void TestSeenTally::markedLowerTaxonMakesItsSpeciesSeen()
{
    const auto s = rollUpSeen(tree(), {}, {10100});
    QVERIFY(s[10100].seen);
    QVERIFY(s[10100].marked);
    QVERIFY(s[1010].seen);
    QCOMPARE(s[1010].seenVia, qint64(10100));
    QVERIFY(s[101].seen);
    QCOMPARE(s[101].seenVia, qint64(1010));
    // Ticked by hand as well: not "through" anything.
    const auto both = rollUpSeen(tree(), {}, {101, 1010});
    QCOMPARE(both[101].seenVia, qint64(0));
}

void TestSeenTally::speciesSeenRollsUp()
{
    const auto s = rollUpSeen(tree(), photographed({100}), {200, 1010});
    QCOMPARE(s[10].speciesSeen, 2);   // 100 photographed, 101 through its subspecies
    QCOMPARE(s[20].speciesSeen, 1);
    QCOMPARE(s[1].speciesSeen, 3);
    QCOMPARE(s[1010].speciesSeen, 0);   // a subspecies isn't a species
}

void TestSeenTally::hybridsAreNotSpecies()
{
    const auto s = rollUpSeen(tree(), {}, {30});
    QVERIFY(s[30].seen);
    QCOMPARE(s[1].speciesSeen, 0);
    const SeenTotals t = tallySeen(tree(), s);
    QCOMPARE(t.speciesSeen, 0);
    QCOMPARE(t.belowSeen, 1);
}

void TestSeenTally::talliesOnlyWhatIsShown()
{
    const auto s = rollUpSeen(tree(), photographed({100}), {101, 200});
    const SeenTotals all = tallySeen(tree(), s);
    QCOMPARE(all.speciesShown, 3);
    QCOMPARE(all.speciesSeen, 3);
    QCOMPARE(all.speciesPhotographed, 1);
    QCOMPARE(all.speciesSeenOnly, 2);
    QCOMPARE(all.belowSeen, 0);

    QList<taxonomy::TreeNode> shown;
    for (const auto &n : tree())
        if (n.inatId == 1 || n.inatId == 10 || n.inatId == 100)
            shown << n;
    const SeenTotals some = tallySeen(shown, s);
    QCOMPARE(some.speciesShown, 1);
    QCOMPARE(some.speciesSeen, 1);
    QCOMPARE(some.speciesSeenOnly, 0);
}

QTEST_GUILESS_MAIN(TestSeenTally)
#include "tst_seentally.moc"
