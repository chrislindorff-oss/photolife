#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "match/MatchEngine.h"
#include "match/PathClassifier.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::match;

class TestPathClassifier : public QObject
{
    Q_OBJECT

private slots:
    void groupFolders();
    void familyGenusSpeciesFolders();
    void infraspeciesFolder();
    void localityAndStagingFolders();
    void unknownForAmbiguous();
    void classifyFoldersUsesTreeContext();
    void faunaCommonNameFolderUnderGroup();
    void commonNameFoldersAreTaxaWhereverTheyAre();
};

void TestPathClassifier::groupFolders()
{
    for (const auto &n : {QStringLiteral("3. MONOCOTYLEDONS"), QStringLiteral("GYMNOSPERMS"),
                          QStringLiteral("2. FERNS & FERN ALLIES"),
                          QStringLiteral("Superfamily BOMBYCOIDEA")}) {
        QCOMPARE(classifyFolderName(n).kind, FolderKind::Group);
    }
}

void TestPathClassifier::familyGenusSpeciesFolders()
{
    const auto fam = classifyFolderName(QStringLiteral("Orchidaceae"));
    QCOMPARE(fam.kind, FolderKind::Taxon);
    QCOMPARE(fam.rank, QStringLiteral("family"));

    // Pre-APG family folder still parses as a family (ancestry hint only).
    QCOMPARE(classifyFolderName(QStringLiteral("Mimosaceae")).rank, QStringLiteral("family"));

    const auto gen = classifyFolderName(QStringLiteral("Diuris"));
    QCOMPARE(gen.kind, FolderKind::Taxon);
    QCOMPARE(gen.rank, QStringLiteral("genus"));
    QCOMPARE(gen.inferredName, QStringLiteral("Diuris"));

    const auto sp = classifyFolderName(QStringLiteral("Diuris pardina"));
    QCOMPARE(sp.rank, QStringLiteral("species"));
    QCOMPARE(sp.inferredName, QStringLiteral("Diuris pardina"));
}

void TestPathClassifier::infraspeciesFolder()
{
    const auto c = classifyFolderName(QStringLiteral("Olearia ramulosa var. stricta"));
    QCOMPARE(c.kind, FolderKind::Taxon);
    QCOMPARE(c.rank, QStringLiteral("infraspecies"));
}

void TestPathClassifier::localityAndStagingFolders()
{
    QCOMPARE(classifyFolderName(QStringLiteral("401 Fulbrooks Road, Dadswells Bridge")).kind,
             FolderKind::Locality);
    QCOMPARE(classifyFolderName(QStringLiteral("To Sort and Upload")).kind, FolderKind::Staging);
    QCOMPARE(classifyFolderName(QStringLiteral("jpg_original")).kind, FolderKind::Staging);
}

void TestPathClassifier::unknownForAmbiguous()
{
    QCOMPARE(classifyFolderName(QStringLiteral("Thompson Road, BRNP")).kind, FolderKind::Locality);
    QCOMPARE(classifyFolderName(QStringLiteral("misc bits and bobs")).kind, FolderKind::Unknown);
}

void TestPathClassifier::classifyFoldersUsesTreeContext()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("Flora Photos"));

    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    touch(root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Diuris/Diuris pardina/"
                                "Diuris pardina - Loc 1-1-2020.jpg"));
    touch(root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Chorizandra/"
                                "Chorizandra - Loc 2-1-2020.jpg"));
    touch(root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Diuris/Diuris pardina/"
                                "Thompson Track, BRNP/x - Loc 3-1-2020.jpg"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    scan::CatalogueWriter writer(db.connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({root}), {root}).ok());

    const int updated = classifyFolders(db.connectionName());
    QVERIFY(updated > 0);

    auto kindOf = [&](const QString &name) {
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.prepare(QStringLiteral("SELECT kind, inferred_rank FROM folder WHERE name = ?"));
        q.addBindValue(name);
        q.exec();
        q.next();
        return std::pair<QString, QString>(q.value(0).toString(), q.value(1).toString());
    };

    QCOMPARE(kindOf(QStringLiteral("3. MONOCOTYLEDONS")).first, QStringLiteral("group"));
    QCOMPARE(kindOf(QStringLiteral("Orchidaceae")), (std::pair<QString, QString>{"taxon", "family"}));
    QCOMPARE(kindOf(QStringLiteral("Diuris")), (std::pair<QString, QString>{"taxon", "genus"}));
    QCOMPARE(kindOf(QStringLiteral("Diuris pardina")),
             (std::pair<QString, QString>{"taxon", "species"}));
    // Bare genus folder with no species inside is still a genus.
    QCOMPARE(kindOf(QStringLiteral("Chorizandra")), (std::pair<QString, QString>{"taxon", "genus"}));
    // Locality nested under a species.
    QCOMPARE(kindOf(QStringLiteral("Thompson Track, BRNP")).first, QStringLiteral("locality"));
}

void TestPathClassifier::faunaCommonNameFolderUnderGroup()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("Fauna Photos"));

    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    touch(root + QStringLiteral("/Birds/Ducks, Geese, and Waterfowl/Pacific Black Duck/"
                                "Pacific Black Duck - Werribee 1-1-2020.jpg"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    scan::CatalogueWriter writer(db.connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({root}), {root}).ok());

    QVERIFY(classifyFolders(db.connectionName()) > 0);

    QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
    q.prepare(QStringLiteral("SELECT kind, inferred_rank, inferred_name FROM folder WHERE name = ?"));
    q.addBindValue(QStringLiteral("Pacific Black Duck"));
    q.exec();
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("taxon"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("species"));
    QCOMPARE(q.value(2).toString(), QStringLiteral("Pacific Black Duck"));
}

// A folder named with a known common name is that taxon wherever it sits
// ("Flame Robin" under "Robins" used to count as a place), filed under its
// scientific name when the name means one species -- and its photos then
// match automatically; a group's common name still goes to review.
void TestPathClassifier::commonNameFoldersAreTaxaWhereverTheyAre()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("Nature"));
    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    touch(root + QStringLiteral("/Birds/Robins/Flame Robin/IMG_0001.jpg"));
    touch(root + QStringLiteral("/Birds/Robins/Superb Fairy-wren/IMG_0002.jpg"));
    touch(root + QStringLiteral("/Birds/Robins/Brown Thornbill \u00B7 Acanthiza pusilla/IMG_0003.jpg"));
    touch(root + QStringLiteral("/Birds/Robins/Lake Wendouree/IMG_0004.jpg"));
    touch(root + QStringLiteral("/Plants/Sun Orchids/IMG_0005.jpg"));

    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(db.connectionName());
    auto add = [&](qint64 id, const char *rank, const char *name, const char *common) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.rank = QLatin1String(rank);
        t.name = QLatin1String(name);
        t.commonName = QLatin1String(common);
        t.vernacular = {t.commonName};
        QVERIFY(store.upsertTaxon(t) > 0);
    };
    add(1, "species", "Petroica phoenicea", "Flame Robin");
    add(2, "species", "Malurus cyaneus", "Superb Fairywren");
    add(3, "species", "Acanthiza pusilla", "Brown Thornbill");
    add(4, "genus", "Thelymitra", "Sun Orchids");

    scan::CatalogueWriter writer(db.connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({root}), {root}).ok());
    QVERIFY(classifyFolders(db.connectionName()) > 0);

    auto folder = [&](const QString &name) {
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.prepare(QStringLiteral("SELECT kind, inferred_rank, inferred_name FROM folder WHERE name = ?"));
        q.addBindValue(name);
        q.exec();
        return q.next() ? QStringList{q.value(0).toString(), q.value(1).toString(), q.value(2).toString()}
                        : QStringList{};
    };
    QCOMPARE(folder(QStringLiteral("Flame Robin")),
             (QStringList{QStringLiteral("taxon"), QStringLiteral("species"), QStringLiteral("Petroica phoenicea")}));
    QCOMPARE(folder(QStringLiteral("Superb Fairy-wren")).value(2), QStringLiteral("Malurus cyaneus"));
    QCOMPARE(folder(QStringLiteral("Brown Thornbill \u00B7 Acanthiza pusilla")).value(2),
             QStringLiteral("Acanthiza pusilla"));
    QCOMPARE(folder(QStringLiteral("Lake Wendouree")).value(0), QStringLiteral("locality"));
    QCOMPARE(folder(QStringLiteral("Sun Orchids")),
             (QStringList{QStringLiteral("taxon"), QString(), QStringLiteral("Sun Orchids")}));

    MatchEngine engine(db.connectionName());
    engine.matchAll();
    auto matched = [&](const QString &baseName) {
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.prepare(QStringLiteral("SELECT t.name, m.status FROM capture_match m JOIN capture c ON c.id = m.capture_id "
                                 "LEFT JOIN taxon t ON t.id = m.taxon_id WHERE c.base_name = ?"));
        q.addBindValue(baseName);
        q.exec();
        return q.next() ? q.value(0).toString() + QLatin1Char('/') + q.value(1).toString() : QString();
    };
    QCOMPARE(matched(QStringLiteral("IMG_0001")), QStringLiteral("Petroica phoenicea/auto"));
    QCOMPARE(matched(QStringLiteral("IMG_0002")), QStringLiteral("Malurus cyaneus/auto"));
    QCOMPARE(matched(QStringLiteral("IMG_0003")), QStringLiteral("Acanthiza pusilla/auto"));
    QCOMPARE(matched(QStringLiteral("IMG_0005")), QStringLiteral("Thelymitra/pending"));
}

QTEST_GUILESS_MAIN(TestPathClassifier)
#include "tst_pathclassifier.moc"
