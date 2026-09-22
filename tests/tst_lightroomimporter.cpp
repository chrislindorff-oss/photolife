#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "lightroom/LightroomImporter.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::lightroom;

namespace {

void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("x");
}

void execOrFail(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery q(db);
    QVERIFY2(q.exec(sql), qPrintable(sql + QStringLiteral(": ") + q.lastError().text()));
}

// Builds a minimal .lrcat-shaped fixture tagging `photoPath` with `keyword`.
void writeLrcatFixture(const QString &lrcatPath, const QString &photoPath, const QString &keyword)
{
    const QString connectionName = QStringLiteral("lrcat-importer-fixture");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(lrcatPath);
        QVERIFY(db.open());

        execOrFail(db, QStringLiteral(
            "CREATE TABLE Adobe_images (id_local INTEGER PRIMARY KEY, rootFile INTEGER)"));
        execOrFail(db, QStringLiteral(
            "CREATE TABLE AgLibraryFile (id_local INTEGER PRIMARY KEY, folder INTEGER, "
            "  baseName TEXT, extension TEXT)"));
        execOrFail(db, QStringLiteral(
            "CREATE TABLE AgLibraryFolder (id_local INTEGER PRIMARY KEY, rootFolder INTEGER, "
            "  pathFromRoot TEXT)"));
        execOrFail(db, QStringLiteral(
            "CREATE TABLE AgLibraryRootFolder (id_local INTEGER PRIMARY KEY, absolutePath TEXT)"));
        execOrFail(db, QStringLiteral(
            "CREATE TABLE AgLibraryKeyword (id_local INTEGER PRIMARY KEY, name TEXT)"));
        execOrFail(db, QStringLiteral(
            "CREATE TABLE AgLibraryKeywordImage (image INTEGER, tag INTEGER)"));

        const QFileInfo info(photoPath);
        QSqlQuery root(db);
        root.prepare(QStringLiteral("INSERT INTO AgLibraryRootFolder VALUES (1, ?)"));
        root.addBindValue(info.absolutePath() + QLatin1Char('/'));
        QVERIFY(root.exec());

        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryFolder VALUES (10, 1, '')"));

        QSqlQuery file(db);
        file.prepare(QStringLiteral("INSERT INTO AgLibraryFile VALUES (100, 10, ?, ?)"));
        file.addBindValue(info.completeBaseName());
        file.addBindValue(info.suffix());
        QVERIFY(file.exec());

        execOrFail(db, QStringLiteral("INSERT INTO Adobe_images VALUES (1000, 100)"));

        QSqlQuery kw(db);
        kw.prepare(QStringLiteral("INSERT INTO AgLibraryKeyword VALUES (1, ?)"));
        kw.addBindValue(keyword);
        QVERIFY(kw.exec());

        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (1000, 1)"));

        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

} // namespace

class TestLightroomImporter : public QObject
{
    Q_OBJECT

private slots:
    void importsOverAThreadAndWritesHint();
    void reportsReaderErrorThroughTheThread();
};

void TestLightroomImporter::importsOverAThreadAndWritesHint()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dbPath = tmp.filePath(QStringLiteral("catalogue.sqlite"));
    const QString lrcatPath = tmp.filePath(QStringLiteral("catalog.lrcat"));
    const QString root = tmp.filePath(QStringLiteral("Library"));

    touch(root + QStringLiteral("/2020-01-01/IMG_0001.jpg"));

    QString photoPath;
    {
        Database db;
        QVERIFY(db.open(dbPath));

        scan::CatalogueWriter writer(db.connectionName());
        scan::FileScanner scanner;
        QVERIFY(writer.sync(scanner.scan({root}), {root}).ok());

        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(q.exec(QStringLiteral("SELECT path FROM rendition")));
        QVERIFY(q.next());
        photoPath = q.value(0).toString();

        taxonomy::TaxonomyStore store(db.connectionName());
        taxonomy::Taxon genus;
        genus.inatId = 100;
        genus.rank = QStringLiteral("genus");
        genus.name = QStringLiteral("Diuris");
        store.upsertTaxon(genus);

        taxonomy::Taxon species;
        species.inatId = 101;
        species.parentInatId = 100;
        species.rank = QStringLiteral("species");
        species.name = QStringLiteral("Diuris pardina");
        store.upsertTaxon(species);
        // db goes out of scope here, releasing its connection before the
        // importer opens its own on the worker thread.
    }

    writeLrcatFixture(lrcatPath, photoPath, QStringLiteral("Diuris pardina"));

    LightroomImporter importer(CatalogueDescriptor::sqlite(dbPath));
    QSignalSpy spy(&importer, &LightroomImporter::finished);
    importer.start(lrcatPath);
    QVERIFY(spy.wait(5000));

    const auto stats =
        qvariant_cast<LightroomImportEngine::Stats>(spy.at(0).at(0));
    QVERIFY2(stats.ok(), qPrintable(stats.error));
    QCOMPARE(stats.photosMatched, 1);
    QCOMPARE(stats.keywordsResolved, 1);
    QCOMPARE(stats.hintsWritten, 1);

    Database verify;
    QVERIFY(verify.open(dbPath));
    QSqlQuery q(QSqlDatabase::database(verify.connectionName(), false));
    QVERIFY(q.exec(QStringLiteral(
        "SELECT t.inat_id FROM capture_keyword_hint ckh JOIN taxon t ON t.id = ckh.taxon_id")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toLongLong(), qlonglong(101));
}

void TestLightroomImporter::reportsReaderErrorThroughTheThread()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dbPath = tmp.filePath(QStringLiteral("catalogue.sqlite"));

    {
        Database db;
        QVERIFY(db.open(dbPath));
    }

    LightroomImporter importer(CatalogueDescriptor::sqlite(dbPath));
    QSignalSpy spy(&importer, &LightroomImporter::finished);
    importer.start(QStringLiteral("/no/such/catalog.lrcat"));
    QVERIFY(spy.wait(5000));

    const auto stats =
        qvariant_cast<LightroomImportEngine::Stats>(spy.at(0).at(0));
    QVERIFY(!stats.ok());
    QVERIFY(!stats.error.isEmpty());
}

QTEST_GUILESS_MAIN(TestLightroomImporter)
#include "tst_lightroomimporter.moc"
