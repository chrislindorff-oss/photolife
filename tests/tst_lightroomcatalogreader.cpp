#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "lightroom/LightroomCatalogReader.h"

using namespace pl::lightroom;

namespace {

// Builds a minimal SQLite file shaped like the handful of Lightroom catalog
// tables/columns LightroomCatalogReader relies on -- not a real .lrcat (no
// sample was available to verify against, see the reader's class comment).
void execOrFail(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery q(db);
    QVERIFY2(q.exec(sql), qPrintable(sql + QStringLiteral(": ") + q.lastError().text()));
}

} // namespace

class TestLightroomCatalogReader : public QObject
{
    Q_OBJECT

private slots:
    void readsKeywordsAndReconstructsPaths();
    void clearErrorOnNonLightroomFile();
    void clearErrorOnMissingFile();
};

void TestLightroomCatalogReader::readsKeywordsAndReconstructsPaths()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString lrcatPath = tmp.filePath(QStringLiteral("catalog.lrcat"));

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("lrcat-fixture-writer"));
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

        // Root 1: absolutePath has a trailing slash, folder's pathFromRoot
        // does too -- the common case.
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryRootFolder VALUES (1, '/Users/test/Pictures/')"));
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryFolder VALUES (10, 1, '2020/Orchids/')"));
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryFile VALUES (100, 10, 'DSC001', 'jpg')"));
        execOrFail(db, QStringLiteral("INSERT INTO Adobe_images VALUES (1000, 100)"));

        // Root 2: absolutePath has NO trailing slash, folder's pathFromRoot
        // has no leading slash -- the other common combination.
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryRootFolder VALUES (2, '/Users/test/Archive')"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryFolder VALUES (20, 2, 'Misc')"));
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryFile VALUES (200, 20, 'IMG002', 'jpg')"));
        execOrFail(db, QStringLiteral("INSERT INTO Adobe_images VALUES (2000, 200)"));

        // Root 3: both sides carry a slash where they meet (double-slash
        // hazard) -- should collapse to one, not two.
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryRootFolder VALUES (3, '/Users/test/Slides/')"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryFolder VALUES (30, 3, '/Ferns/')"));
        execOrFail(db, QStringLiteral(
            "INSERT INTO AgLibraryFile VALUES (300, 30, 'IMG003', 'jpg')"));
        execOrFail(db, QStringLiteral("INSERT INTO Adobe_images VALUES (3000, 300)"));

        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeyword VALUES (1, 'Diuris pardina')"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeyword VALUES (2, 'Anglesea')"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeyword VALUES (3, 'Caladenia')"));

        // Photo 1000 has two keywords, one repeated (dedupe check).
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (1000, 1)"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (1000, 2)"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (1000, 1)"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (2000, 3)"));
        execOrFail(db, QStringLiteral("INSERT INTO AgLibraryKeywordImage VALUES (3000, 3)"));

        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("lrcat-fixture-writer"));

    LightroomCatalogReader reader;
    const QList<LightroomPhotoKeywords> photos = reader.read(lrcatPath);
    QVERIFY2(reader.ok(), qPrintable(reader.error()));
    QCOMPARE(photos.size(), 3);

    QHash<QString, QStringList> byPath;
    for (const auto &p : photos)
        byPath.insert(p.absolutePath, p.keywords);

    QVERIFY(byPath.contains(QStringLiteral("/Users/test/Pictures/2020/Orchids/DSC001.jpg")));
    const QStringList kw1 = byPath.value(QStringLiteral("/Users/test/Pictures/2020/Orchids/DSC001.jpg"));
    QCOMPARE(kw1.size(), 2);   // deduped
    QVERIFY(kw1.contains(QStringLiteral("Diuris pardina")));
    QVERIFY(kw1.contains(QStringLiteral("Anglesea")));

    QVERIFY(byPath.contains(QStringLiteral("/Users/test/Archive/Misc/IMG002.jpg")));
    QVERIFY(byPath.contains(QStringLiteral("/Users/test/Slides/Ferns/IMG003.jpg")));
}

void TestLightroomCatalogReader::clearErrorOnNonLightroomFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("not-a-catalog.db"));

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("non-lrcat-fixture"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        execOrFail(db, QStringLiteral("CREATE TABLE unrelated (id INTEGER)"));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("non-lrcat-fixture"));

    LightroomCatalogReader reader;
    const auto photos = reader.read(path);
    QVERIFY(!reader.ok());
    QVERIFY(!reader.error().isEmpty());
    QVERIFY(photos.isEmpty());
}

void TestLightroomCatalogReader::clearErrorOnMissingFile()
{
    LightroomCatalogReader reader;
    const auto photos = reader.read(QStringLiteral("/no/such/catalog.lrcat"));
    QVERIFY(!reader.ok());
    QVERIFY(!reader.error().isEmpty());
    QVERIFY(photos.isEmpty());
}

QTEST_GUILESS_MAIN(TestLightroomCatalogReader)
#include "tst_lightroomcatalogreader.moc"
