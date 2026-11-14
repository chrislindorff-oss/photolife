#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "inat/InatPresence.h"
#include "inat/PresenceCheck.h"
#include "model/CaptureListModel.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"

using namespace pl;
using namespace pl::inat;
using net::Transport;

namespace {

// About 100 m north per 0.0009 degrees of latitude.
constexpr double kLat = -37.9000, kLon = 144.2000;
double north(double metres)
{
    return kLat + metres / 111320.0;
}

ExistingObservation obs(qint64 id, qint64 taxon, const QString &when, std::optional<double> lat = kLat,
                        std::optional<double> lon = kLon)
{
    ExistingObservation e;
    e.id = id;
    e.taxonId = taxon;
    if (taxon == 101)
        e.taxonAncestorIds = {1, 100};
    if (when.size() > 10)
        e.time = QDateTime::fromString(when, Qt::ISODate);
    e.date = QDate::fromString(when.left(10), Qt::ISODate);
    e.latitude = lat;
    e.longitude = lon;
    return e;
}

PresencePhoto photo(qint64 taxon, const QString &when, std::optional<double> lat = kLat,
                    std::optional<double> lon = kLon)
{
    PresencePhoto p;
    p.captureId = 1;
    p.taxonInatId = taxon;
    if (taxon == 101)
        p.taxonAncestorIds = {1, 100};
    if (when.size() > 10)
        p.taken = QDateTime::fromString(when, Qt::ISODate);
    if (!when.isEmpty())
        p.day = QDate::fromString(when.left(10), Qt::ISODate);
    p.latitude = lat;
    p.longitude = lon;
    return p;
}

PresenceConfidence judge(const PresencePhoto &p, const ExistingObservation &e)
{
    return PresenceMatcher({e}).best(p).confidence;
}

} // namespace

class TestInatPresence : public QObject
{
    Q_OBJECT

private slots:
    void highNeedsTheSameSpeciesWithinAnHourAnd200m();
    void relatedOrUnidentifiedIsAtMostMedium();
    void aPhotoKnownByItsDayAlone();
    void undatedPhotosMatchOnlyTheSpeciesAtTheSpot();
    void missingLocationsLeanOnTheSpecies();
    void farOrOtherDaysAreNoMatch();
    void theBestMatchWins();
    void theTimeAsEnteredCountsToo();

    void checkReadsMarksAndLinks();
    void laterChecksReadOnlyNewObservations();
    void decisionsByHandAreRemembered();
    void undoRemovesLinksButNeverUploads();
    void anExpiredTokenStopsTheCheck();

private:
    void setUpCatalogue();
    int capture(const char *name, qint64 taxon, const char *on, std::optional<double> lat);
    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    PresenceCheck::Result runCheck(bool rereadAll = false);
    QString status(int captureId) const;
    qint64 linkedTo(int captureId) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<net::HttpClient> m_http;
    std::unique_ptr<net::INatClient> m_inat;
    int m_robin = 0, m_robinNoGps = 0, m_wren = 0, m_undated = 0, m_uploaded = 0;
};

// --- the matcher --------------------------------------------------------------

void TestInatPresence::highNeedsTheSameSpeciesWithinAnHourAnd200m()
{
    const ExistingObservation e = obs(10, 101, QStringLiteral("2025-10-18T10:00:00"));
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:59:00"), north(150)), e), PresenceConfidence::High);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T11:01:00"), north(150)), e), PresenceConfidence::Medium);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:30:00"), north(199)), e), PresenceConfidence::High);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:30:00"), north(201)), e), PresenceConfidence::Medium);
    const PresenceHit hit = PresenceMatcher({e}).best(photo(101, QStringLiteral("2025-10-18T10:25:00"), north(140)));
    QCOMPARE(hit.observationId, qint64(10));
    QCOMPARE(hit.reasons, QStringLiteral("same species, 25 min apart, 140 m away"));
}

void TestInatPresence::relatedOrUnidentifiedIsAtMostMedium()
{
    // The photo is a species (101); the observation only its genus (100).
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:00:00")), obs(10, 100, QStringLiteral("2025-10-18T10:05:00"))),
             PresenceConfidence::Medium);
    // Not identified there: only close in time and place counts.
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:00:00")), obs(10, 0, QStringLiteral("2025-10-18T10:20:00"))),
             PresenceConfidence::Medium);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:00:00")), obs(10, 0, QStringLiteral("2025-10-18T15:00:00"))),
             PresenceConfidence::None);
    // Another species: nothing.
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:00:00")), obs(10, 555, QStringLiteral("2025-10-18T10:00:00"))),
             PresenceConfidence::None);
}

void TestInatPresence::aPhotoKnownByItsDayAlone()
{
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18"), north(100)), obs(10, 101, QStringLiteral("2025-10-18T09:00:00"))),
             PresenceConfidence::High);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18"), north(600)), obs(10, 101, QStringLiteral("2025-10-18T09:00:00"))),
             PresenceConfidence::Medium);
}

void TestInatPresence::undatedPhotosMatchOnlyTheSpeciesAtTheSpot()
{
    const ExistingObservation e = obs(10, 101, QStringLiteral("2019-04-01T09:00:00"));
    const PresenceHit hit = PresenceMatcher({e}).best(photo(101, QString(), north(150)));
    QCOMPARE(hit.confidence, PresenceConfidence::Low);
    QVERIFY(hit.reasons.contains(QStringLiteral("photo has no date")));
    QCOMPARE(judge(photo(101, QString(), north(300)), e), PresenceConfidence::None);
    QCOMPARE(judge(photo(100, QString()), e), PresenceConfidence::None);   // related isn't enough
    QCOMPARE(judge(photo(101, QString(), std::nullopt, std::nullopt), e), PresenceConfidence::None);
}

void TestInatPresence::missingLocationsLeanOnTheSpecies()
{
    const ExistingObservation e = obs(10, 101, QStringLiteral("2025-10-18T10:00:00"));
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T14:00:00"), std::nullopt, std::nullopt), e),
             PresenceConfidence::Medium);
    QCOMPARE(judge(photo(100, QStringLiteral("2025-10-18T10:10:00"), std::nullopt, std::nullopt), e),
             PresenceConfidence::Low);
    QCOMPARE(judge(photo(100, QStringLiteral("2025-10-18T14:00:00"), std::nullopt, std::nullopt), e),
             PresenceConfidence::None);
}

void TestInatPresence::farOrOtherDaysAreNoMatch()
{
    const ExistingObservation e = obs(10, 101, QStringLiteral("2025-10-18T10:00:00"));
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-18T10:00:00"), north(1500)), e), PresenceConfidence::None);
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-19T10:00:01")), e), PresenceConfidence::None);   // > 12 hours
    QCOMPARE(judge(photo(101, QStringLiteral("2025-10-20")), e), PresenceConfidence::None);
}

void TestInatPresence::theBestMatchWins()
{
    const PresenceMatcher matcher({obs(10, 100, QStringLiteral("2025-10-18T10:00:00")),
                                   obs(11, 101, QStringLiteral("2025-10-18T10:40:00")),
                                   obs(12, 101, QStringLiteral("2025-10-18T10:10:00"))});
    const PresenceHit hit = matcher.best(photo(101, QStringLiteral("2025-10-18T10:05:00")));
    QCOMPARE(hit.confidence, PresenceConfidence::High);
    QCOMPARE(hit.observationId, qint64(12));   // the closer in time of the two High ones
}

void TestInatPresence::theTimeAsEnteredCountsToo()
{
    // A camera's 13:17 sent as +00:00: iNaturalist shows it as 23:17 in
    // Melbourne, but the time as entered is still the camera's.
    ExistingObservation e = obs(10, 101, QStringLiteral("2015-09-09T23:17:45"));
    QCOMPARE(judge(photo(101, QStringLiteral("2015-09-09T13:20:00")), e), PresenceConfidence::Medium);
    e.timeEntered = QDateTime::fromString(QStringLiteral("2015-09-09T13:17:45"), Qt::ISODate);
    const PresenceHit hit = PresenceMatcher({e}).best(photo(101, QStringLiteral("2015-09-09T13:20:00")));
    QCOMPARE(hit.confidence, PresenceConfidence::High);
    QCOMPARE(hit.secondsApart, qint64(135));
}

// --- the check against the catalogue -----------------------------------------

void TestInatPresence::setUpCatalogue()
{
    // A file, not :memory:: the photo grid's model reads on its own connection.
    m_dir = std::make_unique<QTemporaryDir>();
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(m_dir->filePath(QStringLiteral("catalogue.db"))));
    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const char *rank, const char *name, const char *ancestry) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = QString::fromLatin1(rank);
        t.name = QString::fromLatin1(name);
        t.ancestry = QString::fromLatin1(ancestry);
        store.upsertTaxon(t);
    };
    add(1, 0, "family", "Petroicidae", "");
    add(100, 1, "genus", "Petroica", "1");
    add(101, 100, "species", "Petroica phoenicea", "1/100");
    add(200, 1, "species", "Malurus cyaneus", "1");
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)"));
    m_robin = capture("robin", 101, "2025-10-18T10:05:00", north(50));        // High with obs 10
    m_robinNoGps = capture("robin2", 101, "2025-10-18T13:00:00", std::nullopt); // Medium with obs 10
    m_wren = capture("wren", 200, "2025-10-18T10:00:00", north(10));          // nothing: another species
    m_undated = capture("robin-old", 101, nullptr, north(100));               // Low with obs 10
    m_uploaded = capture("uploaded", 101, "2025-10-18T10:06:00", north(20));  // uploaded from PhotoLife
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("UPDATE capture SET inat_observation_id = 77, inat_photo_id = 9001 WHERE id = %1")
                  .arg(m_uploaded));
}

int TestInatPresence::capture(const char *name, qint64 taxon, const char *on, std::optional<double> lat)
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery c(db);
    c.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name, captured_on, latitude, longitude) "
                             "VALUES (1, ?, ?, ?, ?)"));
    c.addBindValue(QString::fromLatin1(name));
    c.addBindValue(on ? QVariant(QString::fromLatin1(on)) : QVariant());
    c.addBindValue(lat ? QVariant(*lat) : QVariant());
    c.addBindValue(lat ? QVariant(kLon) : QVariant());
    c.exec();
    const int id = c.lastInsertId().toInt();
    QSqlQuery r(db);
    r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    r.addBindValue(id);
    r.addBindValue(QStringLiteral("/lib/%1.jpg").arg(QString::fromLatin1(name)));
    r.exec();
    QSqlQuery m(db);
    m.prepare(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                             "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'file', 1.0, 'auto')"));
    m.addBindValue(id);
    m.addBindValue(taxon);
    m.exec();
    return id;
}

void TestInatPresence::wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
{
    auto owned = std::make_unique<FakeTransport>();
    m_transport = owned.get();
    m_transport->responder = std::move(responder);
    m_http = std::make_unique<net::HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_http->setMaxRetries(0);
    m_inat = std::make_unique<net::INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
}

PresenceCheck::Result TestInatPresence::runCheck(bool rereadAll)
{
    PresenceCheck check(*m_inat, m_db->connectionName());
    QSignalSpy done(&check, &PresenceCheck::finished);
    check.start(QStringLiteral("someone"), rereadAll);
    if (!done.wait(5000))
        return {};
    return done.first().first().value<PresenceCheck::Result>();
}

QString TestInatPresence::status(int captureId) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT status || '/' || confidence FROM capture_inat_status WHERE capture_id = ?"));
    q.addBindValue(captureId);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

qint64 TestInatPresence::linkedTo(int captureId) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT inat_observation_id, inat_photo_id FROM capture WHERE id = ?"));
    q.addBindValue(captureId);
    if (!q.exec() || !q.next() || q.value(0).isNull())
        return 0;
    return q.value(0).toLongLong();
}

namespace {
// Observation 10: a Flame Robin at the spot at 10:00. Then nothing more.
Transport::Reply observations(const Transport::Request &req, int)
{
    if (QUrlQuery(req.url).hasQueryItem(QStringLiteral("id_above")))
        return FakeTransport::ok(R"({"total_results": 0, "results": []})");
    return FakeTransport::ok(R"({"total_results": 1, "results": [
        {"id": 10, "observed_on": "2025-10-18", "time_observed_at": "2025-10-18T10:00:00+11:00",
         "taxon": {"id": 101, "name": "Petroica phoenicea", "preferred_common_name": "Flame Robin",
                   "ancestor_ids": [1, 100, 101]},
         "geojson": {"type": "Point", "coordinates": [144.2, -37.9]}, "place_guess": "Anakie",
         "photos": [{"id": 5, "url": "https://example.test/square.jpg"}]}]})");
}
} // namespace

void TestInatPresence::checkReadsMarksAndLinks()
{
    setUpCatalogue();
    wire(observations);
    const PresenceCheck::Result r = runCheck();
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.observationsRead, 1);
    QCOMPARE(r.photosChecked, 4);   // not the uploaded one
    QCOMPARE(r.marked, 1);
    QCOMPARE(r.medium, 1);
    QCOMPARE(r.low, 1);
    QCOMPARE(status(m_robin), QStringLiteral("on_inat/high"));
    QCOMPARE(linkedTo(m_robin), qint64(10));
    QCOMPARE(status(m_robinNoGps), QStringLiteral("maybe/medium"));
    QCOMPARE(linkedTo(m_robinNoGps), qint64(0));
    QCOMPARE(status(m_undated), QStringLiteral("maybe/low"));
    QVERIFY(status(m_wren).isEmpty());
    // Every request asked for observations without photos too.
    for (const Transport::Request &req : m_transport->received)
        QVERIFY(!QUrlQuery(req.url).hasQueryItem(QStringLiteral("photos")));

    // The review list.
    const QList<PresenceStore::Item> maybe = PresenceStore::items(m_db->connectionName(), QStringLiteral("maybe"));
    QCOMPARE(maybe.size(), 2);
    QCOMPARE(maybe.first().confidence, PresenceConfidence::Medium);
    QCOMPARE(maybe.first().observationTaxon, QStringLiteral("Flame Robin · Petroica phoenicea"));
    QCOMPARE(PresenceStore::items(m_db->connectionName(), QStringLiteral("auto")).size(), 1);

    // To upload leaves out the one on iNaturalist.
    QTemporaryDir thumbs;
    thumb::ThumbnailCache cache(thumbs.path());
    model::CaptureListModel model(*m_db, cache);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.setTodoFilter(model::CaptureListModel::TodoToUpload);
    QVERIFY(reset.wait(2000));
    QStringList shown;
    for (int i = 0; i < model.rowCount(); ++i)
        shown << model.index(i).data(model::CaptureListModel::DisplayNameRole).toString();
    shown.sort();
    QCOMPARE(shown, (QStringList{QStringLiteral("robin-old"), QStringLiteral("wren")}));   // "maybe" stays
}

void TestInatPresence::laterChecksReadOnlyNewObservations()
{
    setUpCatalogue();
    wire(observations);
    QVERIFY(runCheck().ok);
    m_transport->received.clear();
    const PresenceCheck::Result again = runCheck();
    QVERIFY(again.ok);
    QCOMPARE(again.observationsRead, 0);
    QCOMPARE(again.observationsKnown, 1);
    QCOMPARE(m_transport->received.size(), 1);
    QCOMPARE(QUrlQuery(m_transport->received.first().url).queryItemValue(QStringLiteral("id_above")),
             QStringLiteral("10"));
    QCOMPARE(again.photosChecked, 3);   // the High one is settled
    // Re-read all: from the start again.
    m_transport->received.clear();
    QCOMPARE(runCheck(true).observationsRead, 1);
    QVERIFY(!QUrlQuery(m_transport->received.first().url).hasQueryItem(QStringLiteral("id_above")));
}

void TestInatPresence::decisionsByHandAreRemembered()
{
    setUpCatalogue();
    wire(observations);
    const QString conn = m_db->connectionName();
    QVERIFY(PresenceStore::markNotOnInat(conn, {m_robinNoGps}));
    QVERIFY(PresenceStore::markOnInat(conn, {m_wren}, 0, PresenceConfidence::None, {}, QStringLiteral("user")));
    QVERIFY(runCheck().ok);
    QCOMPARE(status(m_robinNoGps), QStringLiteral("not_on_inat/manual"));   // not suggested again
    QCOMPARE(status(m_wren), QStringLiteral("on_inat/manual"));
    QCOMPARE(linkedTo(m_wren), qint64(0));   // no observation given: just marked
    // Yes to a "maybe": linked to its observation.
    QVERIFY(PresenceStore::markOnInat(conn, {m_undated}, 10, PresenceConfidence::Low, QStringLiteral("x"),
                                      QStringLiteral("user")));
    QCOMPARE(linkedTo(m_undated), qint64(10));
    QVERIFY(PresenceStore::clearStatus(conn, {m_undated, m_wren}));
    QCOMPARE(linkedTo(m_undated), qint64(0));
    QVERIFY(status(m_wren).isEmpty());
}

void TestInatPresence::undoRemovesLinksButNeverUploads()
{
    setUpCatalogue();
    wire(observations);
    const PresenceCheck::Result r = runCheck();
    QVERIFY(r.ok);
    QCOMPARE(PresenceStore::lastRun(m_db->connectionName()), r.run);
    QCOMPARE(PresenceStore::undoRun(m_db->connectionName(), r.run), 3);
    QVERIFY(status(m_robin).isEmpty());
    QCOMPARE(linkedTo(m_robin), qint64(0));
    QVERIFY(status(m_undated).isEmpty());
    // Clearing a photo PhotoLife uploaded never forgets its observation.
    QVERIFY(PresenceStore::clearStatus(m_db->connectionName(), {m_uploaded}));
    QCOMPARE(linkedTo(m_uploaded), qint64(77));
}

void TestInatPresence::anExpiredTokenStopsTheCheck()
{
    setUpCatalogue();
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(401); });
    const PresenceCheck::Result r = runCheck();
    QVERIFY(!r.ok);
    QCOMPARE(r.status, 401);
    QVERIFY(status(m_robin).isEmpty());
}

QTEST_MAIN(TestInatPresence)
#include "tst_inatpresence.moc"
