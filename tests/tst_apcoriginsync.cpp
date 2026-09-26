#include <QtTest>

#include <QBuffer>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "taxonomy/ApcOriginSync.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::taxonomy;

namespace {

// A trimmed APC taxon export: an accepted species, an accepted subspecies,
// a quoted multi-line remark, a synonym, and a misapplied name (ignored).
const QByteArray kCsv =
    "taxonID,acceptedNameUsageID,taxonomicStatus,canonicalName,taxonRemarks,taxonDistribution\n"
    "t1,t1,accepted,Arctotheca calendula,,\"WA (naturalised), SA (naturalised), Vic (naturalised)\"\n"
    "t2,t2,accepted,Acacia baileyana,\"Remark, with a comma\nand a second line\","
    "\"NSW (native and naturalised), Vic (naturalised), LHI\"\n"
    "t3,t3,accepted,Acacia longifolia subsp. longifolia,,\"SA (naturalised), NSW, Vic (native and naturalised), Qld (uncertain origin)\"\n"
    "s1,t2,taxonomic synonym,Racosperma baileyanum,,\n"
    "m1,t1,misapplied,Cryptostemma calendulaceum,,\n";

} // namespace

class TestApcOriginSync : public QObject
{
    Q_OBJECT

private slots:
    void parsesDistributionQualifiers();
    void readsAcceptedNamesAndSynonyms();
    void recordsOriginsForCachedTaxa();
};

void TestApcOriginSync::parsesDistributionQualifiers()
{
    const auto d = ApcOriginSync::parseDistribution(QStringLiteral(
        "WA (naturalised), NT (doubtfully naturalised), SA, Qld (native and naturalised), "
        "NSW (formerly naturalised), ACT (uncertain origin), Vic (presumed extinct), LHI, "
        "Tas (naturalised)"));
    QCOMPARE(d.value(QStringLiteral("WA")), QStringLiteral("introduced"));
    QCOMPARE(d.value(QStringLiteral("NT")), QStringLiteral("introduced"));
    QCOMPARE(d.value(QStringLiteral("SA")), QStringLiteral("native"));
    QCOMPARE(d.value(QStringLiteral("Qld")), QStringLiteral("native"));
    QCOMPARE(d.value(QStringLiteral("NSW")), QStringLiteral("introduced"));
    QVERIFY(!d.contains(QStringLiteral("ACT")));   // uncertain origin: unknown
    QCOMPARE(d.value(QStringLiteral("Vic")), QStringLiteral("native"));
    QVERIFY(!d.contains(QStringLiteral("LHI")));   // offshore island: not a state
    QCOMPARE(d.value(QStringLiteral("Tas")), QStringLiteral("introduced"));
    QCOMPARE(ApcOriginSync::placeForState(QStringLiteral("Vic")).value_or(0), qint64(7830));
    QVERIFY(!ApcOriginSync::placeForState(QStringLiteral("LHI")));
}

void TestApcOriginSync::readsAcceptedNamesAndSynonyms()
{
    QBuffer buffer;
    buffer.setData(kCsv);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    const auto byName = ApcOriginSync::readTaxonCsv(buffer);

    QVERIFY(byName.value(QStringLiteral("arctotheca calendula")).contains(QStringLiteral("SA")));
    // Quoted, multi-line remark didn't break the record.
    QVERIFY(byName.value(QStringLiteral("acacia baileyana"))
                .startsWith(QStringLiteral("NSW (native and naturalised)")));
    // Rank connector dropped, as iNaturalist writes trinomials.
    QVERIFY(byName.contains(QStringLiteral("acacia longifolia longifolia")));
    // A synonym carries its accepted taxon's distribution; misapplied names don't count.
    QCOMPARE(byName.value(QStringLiteral("racosperma baileyanum")),
             byName.value(QStringLiteral("acacia baileyana")));
    QVERIFY(!byName.contains(QStringLiteral("cryptostemma calendulaceum")));
}

void TestApcOriginSync::recordsOriginsForCachedTaxa()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("export.csv"));
    {
        QFile f(source);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kCsv);
    }

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    TaxonomyStore store(db.connectionName());
    auto add = [&](qint64 id, const QString &rank, const QString &name) {
        Taxon t;
        t.inatId = id;
        t.rank = rank;
        t.name = name;
        store.upsertTaxon(t);
    };
    add(1, QStringLiteral("species"), QStringLiteral("Arctotheca calendula"));
    add(2, QStringLiteral("species"), QStringLiteral("Acacia baileyana"));
    add(3, QStringLiteral("subspecies"), QStringLiteral("Acacia longifolia longifolia"));
    add(4, QStringLiteral("genus"), QStringLiteral("Acacia"));   // not a leaf: never matched

    ApcOriginSync sync(store, dir.filePath(QStringLiteral("cache")), QByteArrayLiteral("test"));
    sync.setDownloadUrl(QUrl::fromLocalFile(source));
    QSignalSpy spy(&sync, &ApcOriginSync::finished);
    sync.start();
    QVERIFY(spy.wait(10000));
    QVERIFY2(spy.at(0).at(0).toBool(), qPrintable(spy.at(0).at(1).toString()));
    QCOMPARE(spy.at(0).at(2).toInt(), 3);
    QVERIFY(QFile::exists(sync.csvPath()));   // kept for re-matching

    auto origin = [&](qint64 inatId, qint64 place) {
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.prepare(QStringLiteral(
            "SELECT o.origin FROM taxon_origin o JOIN taxon t ON t.id = o.taxon_id "
            "WHERE t.inat_id = ? AND o.place_inat_id = ? AND o.source = 'APC'"));
        q.addBindValue(inatId);
        q.addBindValue(place);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    };
    QCOMPARE(origin(1, 7830), QStringLiteral("introduced"));   // Vic
    QCOMPARE(origin(2, 6825), QStringLiteral("native"));       // NSW: native and naturalised
    QCOMPARE(origin(2, 7830), QStringLiteral("introduced"));
    QCOMPARE(origin(3, 6899), QStringLiteral("introduced"));   // SA
    QVERIFY(origin(3, 7308).isEmpty());                        // Qld: uncertain origin

    // Re-running replaces rather than duplicates (from the kept file, no download).
    QFile::remove(source);
    sync.start();
    QVERIFY(spy.wait(10000));
    QVERIFY(spy.at(1).at(0).toBool());
    QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM taxon_origin")) && q.next());
    QCOMPARE(q.value(0).toInt(), 8);
}

QTEST_GUILESS_MAIN(TestApcOriginSync)
#include "tst_apcoriginsync.moc"
