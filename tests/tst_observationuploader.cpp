#include <QtTest>

#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "FakeTransport.h"
#include "db/Database.h"
#include "inat/ObservationUploader.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"

using namespace pl;
using namespace pl::net;
using namespace pl::inat;

class TestObservationUploader : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void sendsTheObservationAndEveryPhoto();
    void aFailedPhotoIsTheOnlyOneSentAgain();
    void aCreateThatMayHaveWorkedIsLookedUpNotRepeated();
    void anExpiredTokenStopsTheRun();
    void linkedPhotosAreNeverSent();
    void addingToAnObservationOnlySendsPhotos();
    void photosGoInTheOrderGiven();
    void resetForgetsWhatIsntThereAnyMore();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<ObservationUploader> m_uploader;

    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    ObservationUploader::Job job(int photos);
    QList<std::pair<qint64, qint64>> stamped();   // (observation, photo) per capture, by id
    int posts(const QString &path) const;
};

void TestObservationUploader::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)")));
    for (int i = 1; i <= 3; ++i) {
        QVERIFY(q.exec(QStringLiteral("INSERT INTO capture (id, folder_id, base_name) VALUES (%1, 1, 'IMG_%1')").arg(i)));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QVERIFY(image.save(m_dir->filePath(QStringLiteral("IMG_%1.jpg").arg(i))));
    }
}

void TestObservationUploader::cleanup()
{
    m_uploader.reset();
    m_inat.reset();
    m_http.reset();
    m_db.reset();
    m_dir.reset();
}

void TestObservationUploader::wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    m_uploader.reset();
    m_inat.reset();
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(responder);
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));
    m_uploader = std::make_unique<ObservationUploader>(*m_inat, m_db->connectionName());
}

ObservationUploader::Job TestObservationUploader::job(int photos)
{
    ObservationUploader::Job j;
    j.key = 7;
    j.geoprivacy = QStringLiteral("open");
    j.observation.uuid = QStringLiteral("u-1");
    j.observation.taxon.inatId = 12936;
    j.observation.taxon.name = QStringLiteral("Petroica phoenicea");
    for (int i = 1; i <= photos; ++i) {
        UploadPhoto p;
        p.captureId = i;
        p.path = m_dir->filePath(QStringLiteral("IMG_%1.jpg").arg(i));
        p.label = QStringLiteral("IMG_%1.CR3").arg(i);
        p.taken = QDateTime(QDate(2025, 10, 18), QTime(8, i));
        p.hasTime = true;
        p.latitude = -37.83;
        p.longitude = 144.98;
        j.observation.photos << p;
    }
    return j;
}

QList<std::pair<qint64, qint64>> TestObservationUploader::stamped()
{
    QList<std::pair<qint64, qint64>> out;
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT inat_observation_id, inat_photo_id FROM capture ORDER BY id"));
    while (q.next())
        out.append({q.value(0).toLongLong(), q.value(1).toLongLong()});
    return out;
}

int TestObservationUploader::posts(const QString &path) const
{
    int n = 0;
    for (const Transport::Request &r : std::as_const(m_transport->received))
        if (r.verb == "POST" && r.url.path().endsWith(path))
            ++n;
    return n;
}

void TestObservationUploader::sendsTheObservationAndEveryPhoto()
{
    int photoId = 4400;
    wire([&photoId](const Transport::Request &req, int) {
        if (req.url.path().endsWith(QStringLiteral("/observations")))
            return FakeTransport::ok(R"({"id": 901})");
        return FakeTransport::ok(QStringLiteral(R"({"photo": {"id": %1}})").arg(++photoId).toUtf8());
    });
    QSignalSpy made(m_uploader.get(), &ObservationUploader::observationMade);
    QSignalSpy sent(m_uploader.get(), &ObservationUploader::photoSent);
    QSignalSpy finished(m_uploader.get(), &ObservationUploader::jobFinished);
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({job(2)});
    QTRY_COMPARE(all.size(), 1);
    QVERIFY(!all.at(0).at(0).toBool());
    QCOMPARE(made.size(), 1);
    QCOMPARE(made.at(0).at(1).toLongLong(), 901);
    QCOMPARE(sent.size(), 2);
    QCOMPARE(finished.at(0).at(1).toString(), QString());
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{901, 4401}, {901, 4402}, {0, 0}}));

    const QJsonObject o = QJsonDocument::fromJson(m_transport->received.at(0).body).object()
                              .value(QStringLiteral("observation")).toObject();
    QCOMPARE(o.value(QStringLiteral("observed_on_string")).toString(), QStringLiteral("2025-10-18 08:01:00"));
    QCOMPARE(o.value(QStringLiteral("species_guess")).toString(), QStringLiteral("Petroica phoenicea"));
    // The photo goes as a JPEG named after the file, its metadata left behind.
    const QByteArray body = m_transport->received.at(1).body;
    QVERIFY(body.contains("filename=\"IMG_1.jpg\""));
    QVERIFY(body.contains("\xFF\xD8"));
}

void TestObservationUploader::aFailedPhotoIsTheOnlyOneSentAgain()
{
    wire([](const Transport::Request &req, int call) {
        if (req.url.path().endsWith(QStringLiteral("/observations")))
            return FakeTransport::ok(R"({"id": 901})");
        if (call == 2)
            return FakeTransport::httpStatus(422, R"({"error": "File is too big"})");
        return FakeTransport::ok(R"({"photo": {"id": 4401}})");
    });
    QSignalSpy finished(m_uploader.get(), &ObservationUploader::jobFinished);
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({job(2)});
    QTRY_COMPARE(all.size(), 1);
    QVERIFY(finished.at(0).at(1).toString().contains(QStringLiteral("File is too big")));
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{901, 4401}, {901, 0}, {0, 0}}));

    // Retry, as the page does it: from what the catalogue now says.
    ObservationUploader::Job retry = job(2);
    retry.observation.observationId = 901;
    for (UploadPhoto &p : retry.observation.photos)
        p.observationId = 901;
    retry.observation.photos[0].photoId = 4401;
    wire([](const Transport::Request &, int) { return FakeTransport::ok(R"({"photo": {"id": 4402}})"); });
    QSignalSpy again(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({retry});
    QTRY_COMPARE(again.size(), 1);
    QCOMPARE(m_transport->received.size(), 1);
    QCOMPARE(posts(QStringLiteral("/observation_photos")), 1);
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{901, 4401}, {901, 4402}, {0, 0}}));
}

void TestObservationUploader::aCreateThatMayHaveWorkedIsLookedUpNotRepeated()
{
    // The create's answer is lost, but it was made: found by its uuid.
    wire([](const Transport::Request &req, int) {
        if (req.verb == "GET")
            return FakeTransport::ok(R"({"total_results": 1, "results": [{"id": 901}]})");
        if (req.url.path().endsWith(QStringLiteral("/observations"))) {
            Transport::Reply lost;
            lost.error = QStringLiteral("connection reset");
            return lost;
        }
        return FakeTransport::ok(R"({"photo": {"id": 4401}})");
    });
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({job(1)});
    QTRY_COMPARE(all.size(), 1);
    QCOMPARE(posts(QStringLiteral("/observations")), 1);
    QVERIFY(m_transport->received.at(1).url.query().contains(QStringLiteral("uuid=u-1")));
    QCOMPARE(stamped().at(0), (std::pair<qint64, qint64>{901, 4401}));

    // Tried before and not there: looked up first, then made.
    ObservationUploader::Job retry = job(1);
    retry.observation.uuidSent = true;
    wire([](const Transport::Request &req, int) {
        if (req.verb == "GET")
            return FakeTransport::ok(R"({"total_results": 0, "results": []})");
        if (req.url.path().endsWith(QStringLiteral("/observations")))
            return FakeTransport::ok(R"({"id": 902})");
        return FakeTransport::ok(R"({"photo": {"id": 4402}})");
    });
    QSignalSpy again(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({retry});
    QTRY_COMPARE(again.size(), 1);
    QCOMPARE(m_transport->received.at(0).verb, QByteArray("GET"));
    QCOMPARE(posts(QStringLiteral("/observations")), 1);
    QCOMPARE(stamped().at(0), (std::pair<qint64, qint64>{902, 4402}));
}

void TestObservationUploader::anExpiredTokenStopsTheRun()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(401); });
    QSignalSpy finished(m_uploader.get(), &ObservationUploader::jobFinished);
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    ObservationUploader::Job second = job(1);
    second.key = 8;
    m_uploader->start({job(1), second});
    QTRY_COMPARE(all.size(), 1);
    QVERIFY(all.at(0).at(0).toBool());
    QCOMPARE(finished.size(), 1);   // the second wasn't tried
    QCOMPARE(finished.at(0).at(2).toInt(), 401);
    QCOMPARE(m_transport->received.size(), 1);
    QVERIFY(!m_uploader->isRunning());
}

void TestObservationUploader::linkedPhotosAreNeverSent()
{
    QVERIFY(ObservationUploader::link(m_db->connectionName(), {1, 2}, 901));
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{901, -1}, {901, -2}, {0, 0}}));
    const QList<UploadPhoto> loaded = ObservationUploader::loadPhotos(m_db->connectionName(), {1, 3});
    for (const UploadPhoto &p : loaded) {
        QCOMPARE(p.linked, p.captureId == 1);
        QCOMPARE(p.observationId, p.captureId == 1 ? 901 : 0);
        QCOMPARE(p.photoId, 0);
    }

    wire([](const Transport::Request &, int) { return FakeTransport::ok(R"({"photo": {"id": 1}})"); });
    ObservationUploader::Job linked = job(2);
    linked.observation.observationId = 901;
    for (UploadPhoto &p : linked.observation.photos) {
        p.observationId = 901;
        p.linked = true;
    }
    QSignalSpy finished(m_uploader.get(), &ObservationUploader::jobFinished);
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({linked});
    QTRY_COMPARE(all.size(), 1);
    QCOMPARE(finished.at(0).at(1).toString(), QString());
    QVERIFY(m_transport->received.isEmpty());
}

void TestObservationUploader::addingToAnObservationOnlySendsPhotos()
{
    int photoId = 4400;
    wire([&photoId](const Transport::Request &, int) {
        return FakeTransport::ok(QStringLiteral(R"({"photo": {"id": %1}})").arg(++photoId).toUtf8());
    });
    ObservationUploader::Job addTo = job(2);
    addTo.observation.observationId = 555;   // "Add These Photos to It"
    for (UploadPhoto &p : addTo.observation.photos)
        p.observationId = 555;
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({addTo});
    QTRY_COMPARE(all.size(), 1);
    QCOMPARE(posts(QStringLiteral("/observations")), 0);
    QCOMPARE(posts(QStringLiteral("/observation_photos")), 2);
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{555, 4401}, {555, 4402}, {0, 0}}));
}

void TestObservationUploader::photosGoInTheOrderGiven()
{
    wire([](const Transport::Request &req, int) {
        if (req.url.path().endsWith(QStringLiteral("/observations")))
            return FakeTransport::ok(R"({"id": 901})");
        return FakeTransport::ok(R"({"photo": {"id": 4401}})");
    });
    ObservationUploader::Job j = job(3);
    j.observation.photos.move(2, 0);   // the last taken first: the cover
    QSignalSpy all(m_uploader.get(), &ObservationUploader::finished);
    m_uploader->start({j});
    QTRY_COMPARE(all.size(), 1);
    QStringList order;
    for (const Transport::Request &r : std::as_const(m_transport->received))
        for (int i = 1; i <= 3; ++i)
            if (r.body.contains(QStringLiteral("filename=\"IMG_%1.jpg\"").arg(i).toUtf8()))
                order << QString::number(i);
    QCOMPARE(order, (QStringList{QStringLiteral("3"), QStringLiteral("1"), QStringLiteral("2")}));
    // Still observed when the first was taken.
    const QJsonObject o = QJsonDocument::fromJson(m_transport->received.at(0).body).object()
                              .value(QStringLiteral("observation")).toObject();
    QCOMPARE(o.value(QStringLiteral("observed_on_string")).toString(), QStringLiteral("2025-10-18 08:01:00"));
}

void TestObservationUploader::resetForgetsWhatIsntThereAnyMore()
{
    // 1 and 2 sent to 901 (photos 11, 12); 3 to 902 (photo 13).
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET inat_observation_id = 901, inat_photo_id = 10 + id WHERE id <= 2")));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET inat_observation_id = 902, inat_photo_id = 13 WHERE id = 3")));
    const QList<UploadPhoto> photos = ObservationUploader::loadPhotos(m_db->connectionName(), {1, 2, 3});

    // 902 deleted; 901 still there, without photo 12.
    const ObservationUploader::ResetPlan plan = ObservationUploader::planReset(photos, {{901, {11}}});
    QCOMPARE(plan.goneObservations, (QList<qint64>{902}));
    QCOMPARE(plan.photosGone, (QList<int>{2}));
    QCOMPARE(plan.stillThere, (QList<int>{1}));
    QVERIFY(plan.copies.isEmpty());

    QVERIFY(ObservationUploader::forget(m_db->connectionName(), plan.goneObservations, plan.photosGone));
    QCOMPARE(stamped(), (QList<std::pair<qint64, qint64>>{{901, 11}, {901, 0}, {0, 0}}));
    // 2 goes to 901 again; 3 is new.
    for (const UploadPhoto &p : ObservationUploader::loadPhotos(m_db->connectionName(), {1, 2, 3})) {
        QCOMPARE(p.done(), p.captureId == 1);
        QCOMPARE(p.observationId, p.captureId == 3 ? 0 : 901);
    }
}

QTEST_GUILESS_MAIN(TestObservationUploader)
#include "tst_observationuploader.moc"
