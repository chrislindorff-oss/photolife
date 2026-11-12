// StorageStats: sizing and cleaning PhotoLife's own caches.
#include "app/StorageStats.h"
#include "db/Database.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace pl::app;

namespace {

void writeFile(const QString &path, int bytes, int ageSeconds = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(bytes, 'x'));
    f.close();
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(-ageSeconds), QFileDevice::FileModificationTime));
}

} // namespace

class TestStorageStats : public QObject
{
    Q_OBJECT

private slots:
    void pruneRemovesTheOldestDownToUnderTheLimit();
    void clearEmptiesTheFolderButKeepsIt();
    void unusedThumbnailsGoAndUsedOnesStay();
    void contentHashesAreReadFromACatalogue();
};

void TestStorageStats::pruneRemovesTheOldestDownToUnderTheLimit()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    // Ten 1000-byte tiles, tile 0 the oldest.
    for (int i = 0; i < 10; ++i)
        writeFile(dir.filePath(QStringLiteral("14/%1/7.png").arg(i)), 1000, 1000 - i * 10);
    QCOMPARE(directoryBytes(tmp.path()), qint64(10000));
    // Within the limit: nothing happens.
    QCOMPARE(pruneToSize(tmp.path(), 10000).files, 0);
    QCOMPARE(directoryBytes(tmp.path()), qint64(10000));
    // Over it: down to under 90% of the limit, oldest first.
    const Removed r = pruneToSize(tmp.path(), 6000);
    QCOMPARE(r.files, 5);
    QCOMPARE(r.bytes, qint64(5000));
    QCOMPARE(directoryBytes(tmp.path()), qint64(5000));
    for (int i = 0; i < 10; ++i)
        QCOMPARE(QFile::exists(dir.filePath(QStringLiteral("14/%1/7.png").arg(i))), i >= 5);
    // The folders emptied went too.
    QVERIFY(!QDir(dir.filePath(QStringLiteral("14/0"))).exists());
    QVERIFY(QDir(dir.filePath(QStringLiteral("14/9"))).exists());
    // A folder that isn't there is nothing to prune.
    QCOMPARE(pruneToSize(dir.filePath(QStringLiteral("nowhere")), 10).files, 0);
    QCOMPARE(pruneToSize(QString(), 10).files, 0);
}

void TestStorageStats::clearEmptiesTheFolderButKeepsIt()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeFile(dir.filePath(QStringLiteral("cache/a/b/one.jpg")), 300);
    writeFile(dir.filePath(QStringLiteral("cache/two.jpg")), 200);
    writeFile(dir.filePath(QStringLiteral("beside.txt")), 50);
    const Removed r = clearDirectory(dir.filePath(QStringLiteral("cache")));
    QCOMPARE(r.files, 2);
    QCOMPARE(r.bytes, qint64(500));
    QVERIFY(QDir(dir.filePath(QStringLiteral("cache"))).exists());
    QVERIFY(QDir(dir.filePath(QStringLiteral("cache"))).isEmpty());
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("beside.txt"))));   // nothing outside it
    QCOMPARE(clearDirectory(QString()).files, 0);
}

void TestStorageStats::unusedThumbnailsGoAndUsedOnesStay()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeFile(dir.filePath(QStringLiteral("256/ab/abc123.jpg")), 100);
    writeFile(dir.filePath(QStringLiteral("1024/ab/abc123.jpg")), 400);
    writeFile(dir.filePath(QStringLiteral("256/ff/ffee00.jpg")), 100);
    writeFile(dir.filePath(QStringLiteral("1024/ff/ffee00.jpg")), 400);
    const Removed r = removeThumbnailsExcept(tmp.path(), {QStringLiteral("abc123")});
    QCOMPARE(r.files, 2);
    QCOMPARE(r.bytes, qint64(500));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("256/ab/abc123.jpg"))));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("1024/ab/abc123.jpg"))));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("256/ff/ffee00.jpg"))));
    QVERIFY(!QDir(dir.filePath(QStringLiteral("256/ff"))).exists());
}

void TestStorageStats::contentHashesAreReadFromACatalogue()
{
    QTemporaryDir tmp;
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("catalogue.db"));
    QString hash;
    {
        // A catalogue as the scanner makes it, with one photo.
        QDir().mkpath(QDir(tmp.path()).filePath(QStringLiteral("photos")));
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QVERIFY(image.save(QDir(tmp.path()).filePath(QStringLiteral("photos/DSC_1.jpg")), "JPG"));
        pl::Database db;
        QVERIFY(db.open(path));
        pl::scan::CatalogueWriter writer(db.connectionName());
        pl::scan::FileScanner scanner;
        const QString root = QDir(tmp.path()).filePath(QStringLiteral("photos"));
        QVERIFY(writer.sync(scanner.scan({root}), {root}).ok());
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        QVERIFY(q.exec(QStringLiteral("SELECT content_hash FROM rendition")));
        QVERIFY(q.next());
        hash = q.value(0).toString();
        QVERIFY(!hash.isEmpty());
    }
    QSet<QString> hashes;
    QString error;
    QVERIFY2(catalogueContentHashes(path, &hashes, &error), qPrintable(error));
    QCOMPARE(hashes, QSet<QString>{hash});
    // Unreadable: said so, so the caller removes nothing.
    QVERIFY(!catalogueContentHashes(QDir(tmp.path()).filePath(QStringLiteral("missing.db")), &hashes, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestStorageStats)
#include "tst_storagestats.moc"
