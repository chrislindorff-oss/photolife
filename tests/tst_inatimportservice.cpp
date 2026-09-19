#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>

#include "db/Database.h"
#include "inat/InatImportService.h"
#include "inat/InatPhotoDownloader.h"

using namespace pl;
using namespace pl::inat;

// InatPhotoDownloader uses a real QNetworkAccessManager, but Qt's own
// file:// scheme handler lets it read local files without any actual network
// server -- so these tests exercise the real downloader, not a fake one.
class TestInatImportService : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void downloadsWritesExifAndStampsProvenance();
    void downloadFailureFailsTheRunWithoutCrashing();
    void unsetExifWriterStillSavesTheFile();
    void fallsBackToObservationIdWhenNoNamingDataAvailable();
    void disambiguatesFilenameCollisionsWithinARun();

private:
    std::unique_ptr<QTemporaryDir> m_sourceDir;
    std::unique_ptr<QTemporaryDir> m_destDir;
    QString m_sourceFile;

    ImportItem makeItem(qint64 obsId, qint64 photoId) const;
};

void TestInatImportService::init()
{
    m_sourceDir = std::make_unique<QTemporaryDir>();
    m_destDir = std::make_unique<QTemporaryDir>();

    m_sourceFile = QDir(m_sourceDir->path()).filePath(QStringLiteral("source.jpg"));
    QFile f(m_sourceFile);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral("fake-jpeg-bytes"));
    f.close();
}

void TestInatImportService::cleanup()
{
    m_sourceDir.reset();
    m_destDir.reset();
}

ImportItem TestInatImportService::makeItem(qint64 obsId, qint64 photoId) const
{
    ImportItem item;
    item.observationId = obsId;
    item.photoId = photoId;
    item.downloadUrl = QUrl::fromLocalFile(m_sourceFile).toString();
    item.observedOn = QStringLiteral("2025-01-10");
    item.latitude = -37.8136;
    item.longitude = 144.9631;
    item.taxonName = QStringLiteral("Diuris sp900");
    item.placeGuess = QStringLiteral("Melbourne, VIC");
    return item;
}

void TestInatImportService::downloadsWritesExifAndStampsProvenance()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));

    QString capturedPath;
    ExifFields capturedFields;
    int exifCalls = 0;
    service.setExifWriter([&](const QString &path, const ExifFields &fields, QString *) {
        capturedPath = path;
        capturedFields = fields;
        ++exifCalls;
        return true;
    });

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({makeItem(1, 2)}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const auto saved = spy.at(0).at(2).value<QList<SavedFile>>();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().observationId, qint64(1));
    QCOMPARE(saved.first().photoId, qint64(2));

    const QString expectedPath = QDir(m_destDir->path())
                                     .filePath(QStringLiteral("Diuris sp900 - Melbourne, VIC - 10012025.jpg"));
    QCOMPARE(saved.first().path, expectedPath);
    QVERIFY(QFile::exists(expectedPath));
    QFile written(expectedPath);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArrayLiteral("fake-jpeg-bytes"));

    QCOMPARE(exifCalls, 1);
    QCOMPARE(capturedPath, expectedPath);
    QCOMPARE(capturedFields.author, QStringLiteral("Test Author"));
    QCOMPARE(capturedFields.dateTimeOriginal, QStringLiteral("2025-01-10"));
    QCOMPARE(capturedFields.latitude.value(), -37.8136);
    QCOMPARE(capturedFields.taxonName, QStringLiteral("Diuris sp900"));

    // Provenance stamping: seed a capture/rendition row at that exact path
    // and confirm stampProvenance() links it back to the iNat ids.
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    QSqlDatabase sqlDb = QSqlDatabase::database(db.connectionName(), false);
    QSqlQuery(sqlDb).exec(QStringLiteral(
        "INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)"));
    QSqlQuery cap(sqlDb);
    cap.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name) VALUES (1, 'Diuris sp900 - Melbourne, VIC - 10012025')"));
    QVERIFY(cap.exec());
    const int captureId = cap.lastInsertId().toInt();
    QSqlQuery ren(sqlDb);
    ren.prepare(QStringLiteral(
        "INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    ren.addBindValue(captureId);
    ren.addBindValue(expectedPath);
    QVERIFY(ren.exec());

    QCOMPARE(InatImportService::stampProvenance(db.connectionName(), saved), 1);

    QSqlQuery check(sqlDb);
    check.prepare(
        QStringLiteral("SELECT inat_observation_id, inat_photo_id FROM capture WHERE id = ?"));
    check.addBindValue(captureId);
    QVERIFY(check.exec() && check.next());
    QCOMPARE(check.value(0).toLongLong(), qint64(1));
    QCOMPARE(check.value(1).toLongLong(), qint64(2));
}

void TestInatImportService::downloadFailureFailsTheRunWithoutCrashing()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));
    service.setExifWriter([](const QString &, const ExifFields &, QString *) { return true; });

    ImportItem item = makeItem(1, 2);
    item.downloadUrl =
        QUrl::fromLocalFile(QDir(m_sourceDir->path()).filePath(QStringLiteral("missing.jpg")))
            .toString();

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({item}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());
}

void TestInatImportService::unsetExifWriterStillSavesTheFile()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));
    // No setExifWriter() call -- must not crash, and the file must still land.

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({makeItem(3, 4)}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QVERIFY(QFile::exists(QDir(m_destDir->path())
                              .filePath(QStringLiteral("Diuris sp900 - Melbourne, VIC - 10012025.jpg"))));
}

void TestInatImportService::fallsBackToObservationIdWhenNoNamingDataAvailable()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));
    service.setExifWriter([](const QString &, const ExifFields &, QString *) { return true; });

    ImportItem item = makeItem(7, 8);
    item.taxonName.clear();
    item.placeGuess.clear();
    item.observedOn.clear();

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({item}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QVERIFY(QFile::exists(QDir(m_destDir->path()).filePath(QStringLiteral("inat-7-8.jpg"))));
}

void TestInatImportService::disambiguatesFilenameCollisionsWithinARun()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));
    service.setExifWriter([](const QString &, const ExifFields &, QString *) { return true; });

    // Two photos from the same observation share species, place, and date --
    // the same base filename -- and must not silently overwrite each other.
    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({makeItem(1, 2), makeItem(1, 3)}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const QString destDir = m_destDir->path();
    QVERIFY(QFile::exists(
        QDir(destDir).filePath(QStringLiteral("Diuris sp900 - Melbourne, VIC - 10012025.jpg"))));
    QVERIFY(QFile::exists(
        QDir(destDir).filePath(QStringLiteral("Diuris sp900 - Melbourne, VIC - 10012025 - 2.jpg"))));
}

QTEST_MAIN(TestInatImportService)
#include "tst_inatimportservice.moc"
