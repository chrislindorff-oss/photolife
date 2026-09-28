#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"
#include "treefile/TreeFile.h"

using namespace pl;
using namespace pl::treefile;

class TestTreeFile : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip_data();
    void roundTrip();
    void csvQuotesAwkwardText();
    void csvKeepsDetailsInCommentLines();
    void writesCoverageButDoesNotReadItBack();
    void readsHandMadeCsv();
    void readsHandMadeXlsx();
    void rejectsFileWithoutNames();
    void exportsProjectDepthFirstWithFacts();

private:
    static TreeFile sample();
    static void writeText(const QString &path, const QByteArray &text)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text);
    }
};

TreeFile TestTreeFile::sample()
{
    TreeFile file;
    file.name = QStringLiteral("Orchids of Victoria");
    file.rootInatId = 47217;
    file.rootName = QStringLiteral("Orchidaceae");
    file.placeInatId = 7830;
    file.placeName = QStringLiteral("Victoria, AU");
    file.excludedInatIds = {5001, 5002};

    TreeFileRow family;
    family.rank = QStringLiteral("family");
    family.name = QStringLiteral("Orchidaceae");
    family.inatId = 47217;
    family.parentInatId = 48460;
    family.ancestry = QStringLiteral("48460");
    family.rankLevel = 30;
    family.inRegion = false;

    TreeFileRow genus;
    genus.rank = QStringLiteral("genus");
    genus.name = QStringLiteral("Diuris");
    genus.commonName = QStringLiteral("Donkey Orchids");
    genus.inatId = 100;
    genus.parentInatId = 47217;
    genus.ancestry = QStringLiteral("48460/47217");
    genus.rankLevel = 20;
    genus.inRegion = false;

    TreeFileRow species;
    species.rank = QStringLiteral("species");
    species.name = QStringLiteral("Diuris × fastidiosa");
    species.commonName = QStringLiteral("Donkey, \"Leopard\" Orchid");
    species.inatId = 900;
    species.parentInatId = 100;
    species.ancestry = QStringLiteral("48460/47217/100");
    species.rankLevel = 10;
    species.inRegion = true;
    species.fromChecklist = true;
    species.isActive = false;
    species.statuses = {{QStringLiteral("Rare"), QStringLiteral("VBA 2021"), 7830},
                        {QStringLiteral("Vulnerable"), QStringLiteral("EPBC"), std::nullopt}};
    species.origins = {{QStringLiteral("native"), QStringLiteral("APC"), 6744}};

    file.rows = {family, genus, species};
    return file;
}

void TestTreeFile::roundTrip_data()
{
    QTest::addColumn<QString>("suffix");
    QTest::newRow("csv") << QStringLiteral("csv");
    QTest::newRow("xlsx") << QStringLiteral("xlsx");
}

void TestTreeFile::roundTrip()
{
    QFETCH(QString, suffix);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tree.") + suffix);
    const TreeFile original = sample();

    QString error;
    QVERIFY2(writeTreeFile(path, original, &error), qPrintable(error));
    const auto read = readTreeFile(path, &error);
    QVERIFY2(read, qPrintable(error));

    QCOMPARE(read->formatVersion, TreeFile::kFormatVersion);

    QCOMPARE(read->name, original.name);
    QCOMPARE(read->rootInatId, original.rootInatId);
    QCOMPARE(read->rootName, original.rootName);
    QCOMPARE(read->placeInatId, original.placeInatId);
    QCOMPARE(read->placeName, original.placeName);
    QCOMPARE(read->excludedInatIds, original.excludedInatIds);
    QCOMPARE(read->rows.size(), original.rows.size());
    for (qsizetype i = 0; i < original.rows.size(); ++i)
        QVERIFY2(read->rows.at(i) == original.rows.at(i), qPrintable(original.rows.at(i).name));
}

void TestTreeFile::csvQuotesAwkwardText()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tree.csv"));
    QString error;
    QVERIFY(writeTreeFile(path, sample(), &error));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));   // BOM, for Excel
    QVERIFY(bytes.contains("\"Donkey, \"\"Leopard\"\" Orchid\""));
    QVERIFY(bytes.contains("Diuris × fastidiosa"));
}

void TestTreeFile::csvKeepsDetailsInCommentLines()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tree.csv"));
    QString error;
    QVERIFY(writeTreeFile(path, sample(), &error));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    QVERIFY(text.contains(QStringLiteral("# Tree: Orchids of Victoria\n")));
    QVERIFY(text.contains(QStringLiteral("# Place ID: 7830\n")));
    QVERIFY(text.contains(QStringLiteral("# Excluded taxon IDs: 5001 5002\n")));
    // Readable columns come first; the rank columns are filled in.
    QVERIFY(text.contains(QStringLiteral("\nRank,Scientific name,Common name,Kingdom,")));
    // No coverage given: the four coverage columns stay empty.
    QVERIFY(text.contains(QStringLiteral(",Orchidaceae,Diuris,Rare,VBA 2021,native,yes,,,,,900,")));
}

void TestTreeFile::writesCoverageButDoesNotReadItBack()
{
    TreeFile file = sample();
    file.rows[1].coverage = TreeFileRow::Coverage{7, QStringLiteral("2024-08-06"), 12, 3};
    file.rows[2].coverage = TreeFileRow::Coverage{0, QString(), 1, 0};

    QTemporaryDir dir;
    for (const QString suffix : {QStringLiteral("csv"), QStringLiteral("xlsx")}) {
        const QString path = dir.filePath(QStringLiteral("tree.") + suffix);
        QString error;
        QVERIFY(writeTreeFile(path, file, &error));
        const auto read = readTreeFile(path, &error);
        QVERIFY2(read, qPrintable(error));
        for (const TreeFileRow &r : read->rows)
            QVERIFY(!r.coverage);   // one library's photos, not part of the tree
    }

    QFile f(dir.filePath(QStringLiteral("tree.csv")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    QVERIFY(text.contains(QStringLiteral(",Photographed,Photos,Latest photo,Species photographed,")));
    // The genus: photographed, with the species count; the species: no, and
    // no species count on a species row.
    QVERIFY(text.contains(QStringLiteral(",no,yes,7,2024-08-06,3 / 12,100,")));
    QVERIFY(text.contains(QStringLiteral(",yes,no,0,,,900,")));
}

void TestTreeFile::readsHandMadeCsv()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("list.csv"));
    writeText(path, "Species,Conservation Status,Native/Introduced\n"
                    "Diuris pardina,Rare,native\n"
                    "*Disa bracteata,,\n"
                    ",,\n"
                    "Caladenia \"Spider\" sp.,,exotic\n");

    QString error;
    const auto file = readTreeFile(path, &error);
    QVERIFY2(file, qPrintable(error));
    QVERIFY(!file->rootInatId);
    QCOMPARE(file->formatVersion, 0);
    QCOMPARE(file->rows.size(), 3);

    const TreeFileRow &a = file->rows.at(0);
    QCOMPARE(a.name, QStringLiteral("Diuris pardina"));
    QVERIFY(!a.inatId);
    QVERIFY(!a.inRegion);
    QCOMPARE(a.statuses, (QList<TreeFileFact>{{QStringLiteral("Rare"), {}, {}}}));
    QCOMPARE(a.origins, (QList<TreeFileFact>{{QStringLiteral("native"), {}, {}}}));

    QCOMPARE(file->rows.at(1).name, QStringLiteral("Disa bracteata"));
    QCOMPARE(file->rows.at(1).origins.value(0).value, QStringLiteral("introduced"));
    QCOMPARE(file->rows.at(2).origins.value(0).value, QStringLiteral("introduced"));
}

void TestTreeFile::readsHandMadeXlsx()
{
    // Only names, no ids: what a hand-made workbook boils down to. (QXlsx is
    // private to the core library, so the workbook is written by our own
    // writer rather than built cell by cell here.)
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("list.xlsx"));
    TreeFile file;
    TreeFileRow row;
    row.name = QStringLiteral("Diuris pardina");
    file.rows = {row};
    QString error;
    QVERIFY(writeTreeFile(path, file, &error));

    const auto read = readTreeFile(path, &error);
    QVERIFY2(read, qPrintable(error));
    QCOMPARE(read->rows.size(), 1);
    QCOMPARE(read->rows.at(0).name, QStringLiteral("Diuris pardina"));
    QVERIFY(!read->rows.at(0).inatId);
}

void TestTreeFile::rejectsFileWithoutNames()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("bad.csv"));
    writeText(path, "Colour,Size\nred,3\n");
    QString error;
    QVERIFY(!readTreeFile(path, &error));
    QVERIFY(error.contains(QStringLiteral("Scientific name")));
}

void TestTreeFile::exportsProjectDepthFirstWithFacts()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(db.connectionName());

    auto taxon = [&](qint64 id, std::optional<qint64> parent, const char *rank, int level,
                     const char *name, const char *ancestry) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.parentInatId = parent;
        t.rank = QLatin1String(rank);
        t.rankLevel = level;
        t.name = QString::fromUtf8(name);
        t.ancestry = QLatin1String(ancestry);
        QVERIFY(store.upsertTaxon(t) > 0);
    };
    taxon(47217, 48460, "family", 30, "Orchidaceae", "48460");
    taxon(200, 47217, "genus", 20, "Pterostylis", "48460/47217");
    taxon(100, 47217, "genus", 20, "Diuris", "48460/47217");
    taxon(901, 100, "species", 10, "Diuris sulphurea", "48460/47217/100");
    taxon(900, 100, "species", 10, "Diuris pardina", "48460/47217/100");
    taxon(950, 200, "species", 10, "Pterostylis nutans", "48460/47217/200");

    const int pid = store.ensureProject(QStringLiteral("Orchids"), 47217, std::nullopt,
                                        QStringLiteral("inat"));
    for (qint64 id : {47217, 200, 100})
        store.addProjectTaxon(pid, id, false, false);
    for (qint64 id : {901, 900, 950})
        store.addProjectTaxon(pid, id, true, id == 900);
    store.addStatus(900, {7830, QStringLiteral("Rare"), QStringLiteral("VBA 2021")});
    store.addOrigin(950, {std::nullopt, QStringLiteral("introduced"), QStringLiteral("APC")});

    const TreeFile file = exportProject(store, pid, QStringLiteral("Orchids"));
    QCOMPARE(file.rootInatId, std::optional<qint64>(47217));
    QCOMPARE(file.rootName, QStringLiteral("Orchidaceae"));

    QStringList order;
    for (const auto &r : file.rows)
        order << r.name;
    QCOMPARE(order, (QStringList{QStringLiteral("Orchidaceae"), QStringLiteral("Diuris"),
                                 QStringLiteral("Diuris pardina"),
                                 QStringLiteral("Diuris sulphurea"),
                                 QStringLiteral("Pterostylis"),
                                 QStringLiteral("Pterostylis nutans")}));

    const TreeFileRow &pardina = file.rows.at(2);
    QCOMPARE(pardina.ancestry, QStringLiteral("48460/47217/100"));
    QCOMPARE(pardina.rankLevel, std::optional<int>(10));
    QVERIFY(pardina.fromChecklist);
    QCOMPARE(pardina.statuses,
             (QList<TreeFileFact>{{QStringLiteral("Rare"), QStringLiteral("VBA 2021"), 7830}}));
    QCOMPARE(file.rows.at(5).origins.value(0).value, QStringLiteral("introduced"));
}

QTEST_MAIN(TestTreeFile)
#include "tst_treefile.moc"
