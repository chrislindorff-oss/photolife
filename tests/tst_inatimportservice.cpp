#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

#include "db/Database.h"
#include "inat/InatImportService.h"
#include "inat/InatPhotoDownloader.h"

using namespace pl;
using namespace pl::inat;

namespace {

// A tiny local HTTP server that answers each request with the next scripted
// status -- 0 means "accept but never answer", to exercise the transfer
// timeout. Once the script runs out it keeps repeating the last entry.
class ScriptedServer : public QTcpServer
{
public:
    explicit ScriptedServer(QList<int> script) : m_script(std::move(script))
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    m_buffers[socket] += socket->readAll();
                    if (!m_buffers[socket].contains("\r\n\r\n"))
                        return;
                    m_buffers.remove(socket);
                    respond(socket);
                });
            }
        });
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/photo.jpg").arg(serverPort()));
    }
    int requests() const { return m_requests; }

private:
    void respond(QTcpSocket *socket)
    {
        const int status = m_script.value(m_requests, m_script.last());
        ++m_requests;
        if (status == 0)
            return;   // hang
        const QByteArray body = status == 200 ? QByteArrayLiteral("served-bytes") : QByteArray();
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n"
                      + body);
        socket->disconnectFromHost();
    }

    QList<int> m_script;
    int m_requests = 0;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

} // namespace

// InatPhotoDownloader uses a real QNetworkAccessManager, but Qt's own
// file:// scheme handler lets it read local files without any actual network
// server -- so these tests exercise the real downloader, not a fake one. The
// retry tests use ScriptedServer above for failures file:// can't produce.
class TestInatImportService : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void downloadsWritesExifAndStampsProvenance();
    void downloadFailureSkipsThePhotoAndContinues();
    void consecutiveFailuresStopTheRun();
    void downloaderRetriesTransientFailures();
    void downloaderGivesUpAfterMaxAttempts();
    void downloaderDoesNotRetryPermanentFailures();
    void downloaderRetriesAStalledTransfer();
    void unsetExifWriterStillSavesTheFile();
    void fallsBackToObservationIdWhenNoNamingDataAvailable();
    void disambiguatesFilenameCollisionsWithinARun();
    void cancelStopsAfterTheCurrentPhotoAndKeepsWhatCompleted();

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

void TestInatImportService::downloadFailureSkipsThePhotoAndContinues()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));
    service.setExifWriter([](const QString &, const ExifFields &, QString *) { return true; });

    ImportItem missing = makeItem(3, 4);
    missing.downloadUrl =
        QUrl::fromLocalFile(QDir(m_sourceDir->path()).filePath(QStringLiteral("missing.jpg")))
            .toString();

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({makeItem(1, 2), missing, makeItem(5, 6)}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), true);
    const auto saved = spy.at(0).at(2).value<QList<SavedFile>>();
    QCOMPARE(saved.size(), 2);
    QCOMPARE(saved.at(0).photoId, qint64(2));
    QCOMPARE(saved.at(1).photoId, qint64(6));
    const QList<FailedDownload> failures = service.failures();
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().item.photoId, qint64(4));
    QVERIFY(!failures.first().error.isEmpty());
}

void TestInatImportService::consecutiveFailuresStopTheRun()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));

    QList<ImportItem> items;
    for (int i = 0; i < InatImportService::kMaxConsecutiveFailures + 1; ++i) {
        ImportItem item = makeItem(10 + i, 20 + i);
        item.downloadUrl = QUrl::fromLocalFile(QDir(m_sourceDir->path()).filePath(
                                                   QStringLiteral("missing%1.jpg").arg(i)))
                               .toString();
        items << item;
    }
    items << makeItem(1, 2);   // never reached

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start(items, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    QVERIFY(spy.at(0).at(2).value<QList<SavedFile>>().isEmpty());
    QCOMPARE(service.failures().size(), InatImportService::kMaxConsecutiveFailures);
    QVERIFY(!service.isRunning());
}

void TestInatImportService::downloaderRetriesTransientFailures()
{
    ScriptedServer server({503, 429, 200});
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    downloader.setRetryPolicy(4, 10);

    const QString dest = QDir(m_destDir->path()).filePath(QStringLiteral("retried.jpg"));
    bool done = false, ok = false;
    downloader.download(server.url(), dest, [&](bool success, const QString &) {
        ok = success;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(ok);
    QCOMPARE(server.requests(), 3);
    QFile file(dest);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("served-bytes"));
}

void TestInatImportService::downloaderGivesUpAfterMaxAttempts()
{
    ScriptedServer server({503});
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    downloader.setRetryPolicy(3, 10);

    const QString dest = QDir(m_destDir->path()).filePath(QStringLiteral("never.jpg"));
    bool done = false, ok = true;
    QString error;
    downloader.download(server.url(), dest, [&](bool success, const QString &e) {
        ok = success;
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(!ok);
    QCOMPARE(server.requests(), 3);
    QVERIFY2(error.contains(QStringLiteral("after 3 attempt")), qPrintable(error));
    QVERIFY(!QFile::exists(dest));
}

void TestInatImportService::downloaderDoesNotRetryPermanentFailures()
{
    ScriptedServer server({404, 200});
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    downloader.setRetryPolicy(4, 10);

    bool done = false, ok = true;
    downloader.download(server.url(), QDir(m_destDir->path()).filePath(QStringLiteral("x.jpg")),
                        [&](bool success, const QString &) {
                            ok = success;
                            done = true;
                        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(!ok);
    QCOMPARE(server.requests(), 1);

    QVERIFY(InatPhotoDownloader::isTransient(QNetworkReply::HostNotFoundError, 0));
    QVERIFY(InatPhotoDownloader::isTransient(QNetworkReply::RemoteHostClosedError, 0));
    QVERIFY(!InatPhotoDownloader::isTransient(QNetworkReply::ContentNotFoundError, 0));
    QVERIFY(InatPhotoDownloader::isTransient(QNetworkReply::UnknownServerError, 502));
    QVERIFY(!InatPhotoDownloader::isTransient(QNetworkReply::ContentAccessDenied, 403));
}

void TestInatImportService::downloaderRetriesAStalledTransfer()
{
    ScriptedServer server({0, 200});   // first request never answered
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    downloader.setRetryPolicy(3, 10);
    downloader.setTransferTimeoutMs(300);

    bool done = false, ok = false;
    downloader.download(server.url(),
                        QDir(m_destDir->path()).filePath(QStringLiteral("stalled.jpg")),
                        [&](bool success, const QString &) {
                            ok = success;
                            done = true;
                        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(ok);
    QCOMPARE(server.requests(), 2);
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

void TestInatImportService::cancelStopsAfterTheCurrentPhotoAndKeepsWhatCompleted()
{
    InatPhotoDownloader downloader(QByteArrayLiteral("test/1"));
    InatImportService service(downloader, QStringLiteral("Test Author"));

    // Cancel as soon as the first photo has landed.
    connect(&service, &InatImportService::progress, &service, [&](int done, int) {
        if (done == 1)
            service.cancel();
    });

    QSignalSpy spy(&service, &InatImportService::finished);
    service.start({makeItem(1, 2), makeItem(3, 4), makeItem(5, 6)}, m_destDir->path());
    QVERIFY(spy.wait(5000));

    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    const auto saved = spy.at(0).at(2).value<QList<SavedFile>>();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().photoId, qint64(2));
    QVERIFY(QFile::exists(saved.first().path));
    QVERIFY(!service.isRunning());
}

QTEST_MAIN(TestInatImportService)
#include "tst_inatimportservice.moc"
