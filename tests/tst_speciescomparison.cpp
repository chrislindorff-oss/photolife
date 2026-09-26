#include <QtTest>

#include "inat/SpeciesComparison.h"

using namespace pl;
using namespace pl::inat;
using pl::taxonomy::Taxon;

namespace {

// Orchidaceae (47217) > Diurideae tribe (1) > Glossodia genus (2) > G. major (83357)
//                                          > Caladenia genus (3) > C. major (100)
//                                                                  > C. major ssp. (101)
//                                                                  > C. minor (102)
const QString kRoot = QStringLiteral("48460/47126/47217");

Taxon taxon(qint64 id, const QString &rank, const QString &name, const QString &ancestry,
            const QString &common = QString())
{
    Taxon t;
    t.inatId = id;
    t.rank = rank;
    t.name = name;
    t.commonName = common;
    t.ancestry = ancestry;
    return t;
}

QHash<qint64, Taxon> knownTaxa()
{
    QHash<qint64, Taxon> k;
    for (const Taxon &t : {
             taxon(47217, QStringLiteral("family"), QStringLiteral("Orchidaceae"),
                   QStringLiteral("48460/47126")),
             taxon(1, QStringLiteral("tribe"), QStringLiteral("Diurideae"), kRoot),
             taxon(2, QStringLiteral("genus"), QStringLiteral("Glossodia"), kRoot + "/1"),
             taxon(3, QStringLiteral("genus"), QStringLiteral("Caladenia"), kRoot + "/1"),
             taxon(83357, QStringLiteral("species"), QStringLiteral("Glossodia major"),
                   kRoot + "/1/2", QStringLiteral("Wax-lip Orchid")),
             taxon(100, QStringLiteral("species"), QStringLiteral("Caladenia major"), kRoot + "/1/3"),
             taxon(102, QStringLiteral("species"), QStringLiteral("Caladenia minor"), kRoot + "/1/3"),
         })
        k.insert(t.inatId, t);
    return k;
}

net::SpeciesCount observed(const Taxon &t, int count)
{
    net::SpeciesCount sc;
    sc.taxon = t;
    sc.count = count;
    return sc;
}

const ComparisonRow *row(const ComparisonResult &r, qint64 id)
{
    for (const ComparisonRow &row : r.rows) {
        if (row.inatId == id)
            return &row;
    }
    return nullptr;
}

} // namespace

class TestSpeciesComparison : public QObject
{
    Q_OBJECT

private slots:
    void rollUpFindsTaxonAtRank();
    void compareMergesBothSidesAtSpeciesRank();
    void compareRollsUpToGenus();
};

void TestSpeciesComparison::rollUpFindsTaxonAtRank()
{
    const QHash<qint64, Taxon> known = knownTaxa();
    const Taxon ssp = taxon(101, QStringLiteral("subspecies"),
                            QStringLiteral("Caladenia major major"), kRoot + "/1/3/100");

    QCOMPARE(rollUp(ssp, known, QStringLiteral("species")).value_or(0), qint64(100));
    QCOMPARE(rollUp(ssp, known, QStringLiteral("genus")).value_or(0), qint64(3));
    QCOMPARE(rollUp(known.value(83357), known, QStringLiteral("species")).value_or(0),
             qint64(83357));
    QCOMPARE(rollUp(known.value(83357), known, QStringLiteral("family")).value_or(0),
             qint64(47217));
    // A genus-level record has no species; a rank the lineage skips has no match.
    QVERIFY(!rollUp(known.value(3), known, QStringLiteral("species")));
    QVERIFY(!rollUp(known.value(83357), known, QStringLiteral("subfamily")));
    // A hybrid counts as itself at species rank, and still rolls up to its genus.
    const Taxon hybrid = taxon(200, QStringLiteral("hybrid"), QStringLiteral("Caladenia × tutelata"),
                               kRoot + "/1/3");
    QCOMPARE(rollUp(hybrid, known, QStringLiteral("species")).value_or(0), qint64(200));
    QCOMPARE(rollUp(hybrid, known, QStringLiteral("genus")).value_or(0), qint64(3));
}

void TestSpeciesComparison::compareMergesBothSidesAtSpeciesRank()
{
    const QHash<qint64, Taxon> known = knownTaxa();
    const QList<net::SpeciesCount> inat = {
        observed(known.value(83357), 5),
        observed(taxon(101, QStringLiteral("subspecies"), QStringLiteral("Caladenia major major"),
                       kRoot + "/1/3/100"),
                 2),
        observed(known.value(3), 4),   // identified only to genus
    };
    const QList<qint64> tree = {100, 102};
    const auto inLibrary = [](qint64 id, const QString &, const QString &) { return id == 102; };

    const ComparisonResult r =
        compare(inat, tree, known, QStringLiteral("species"), inLibrary);

    QCOMPARE(r.rows.size(), 3);
    QCOMPARE(r.unplacedTaxa, 1);
    QCOMPARE(r.unplacedObservations, 4);
    // Name-sorted.
    QCOMPARE(r.rows.at(0).name, QStringLiteral("Caladenia major"));
    QCOMPARE(r.rows.at(2).name, QStringLiteral("Glossodia major"));

    const ComparisonRow *glossodia = row(r, 83357);
    QVERIFY(glossodia);
    QVERIFY(glossodia->onInat && !glossodia->inTree && !glossodia->inLibrary);
    QCOMPARE(glossodia->inatObservations, 5);
    QCOMPARE(glossodia->commonName, QStringLiteral("Wax-lip Orchid"));

    const ComparisonRow *major = row(r, 100);   // observed only as its subspecies
    QVERIFY(major);
    QVERIFY(major->onInat && major->inTree);
    QCOMPARE(major->inatObservations, 2);

    const ComparisonRow *minor = row(r, 102);
    QVERIFY(minor);
    QVERIFY(!minor->onInat && minor->inTree && minor->inLibrary);
    QCOMPARE(minor->inatObservations, 0);
}

void TestSpeciesComparison::compareRollsUpToGenus()
{
    const QHash<qint64, Taxon> known = knownTaxa();
    const QList<net::SpeciesCount> inat = {
        observed(known.value(100), 3),
        observed(known.value(102), 1),
        observed(known.value(3), 4),   // a genus-level record counts at genus rank
    };
    const ComparisonResult r =
        compare(inat, {83357}, known, QStringLiteral("genus"), {});

    QCOMPARE(r.rows.size(), 2);
    QCOMPARE(r.unplacedTaxa, 0);
    const ComparisonRow *caladenia = row(r, 3);
    QVERIFY(caladenia);
    QVERIFY(caladenia->onInat && !caladenia->inTree);
    QCOMPARE(caladenia->inatObservations, 8);
    const ComparisonRow *glossodia = row(r, 2);
    QVERIFY(glossodia);
    QVERIFY(!glossodia->onInat && glossodia->inTree);
}

QTEST_MAIN(TestSpeciesComparison)
#include "tst_speciescomparison.moc"
