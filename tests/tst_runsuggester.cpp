#include <QtTest>

#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "FakeTransport.h"
#include "db/Database.h"
#include "inat/PhotoIdentifier.h"
#include "inat/RunSuggester.h"
#include "match/MatchEngine.h"
#include "match/MatchReviewer.h"
#include "model/ReviewQueueModel.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"

using namespace pl;
using inat::RunSuggester;
using net::Transport;

namespace {
const QByteArray kFlameRobin = R"({"total_results": 1, "results": [
    {"combined_score": 91.0, "vision_score": 88.0, "frequency_score": 1,
     "taxon": {"id": 12936, "rank": "species", "name": "Petroica phoenicea",
               "preferred_common_name": "Flame Robin"}}]})";
}

class TestRunSuggester : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void splitsIntoRunsByTheGapBetweenShots();
    void asksOncePerRunAndGivesEveryPhotoTheSuggestion();
    void matchLibraryKeepsTheSuggestion();
    void confirmingASuggestionLearnsNoAlias();
    void anExpiredTokenStopsTheRun();

private:
    void wire(std::function<Transport::Reply(const Transport::Request &, int)> responder);
    qint64 captureId(const QString &base) const;
    int scoreRequests() const;
    RunSuggester::Result runAll();
    QList<qint64> allCaptures() const;
    qint64 suggestionInat(qint64 captureId) const;

    QTemporaryDir m_tmp;
    QString m_root;
    std::unique_ptr<Database> m_db;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<net::HttpClient> m_http;
    std::unique_ptr<net::INatClient> m_inat;
    std::unique_ptr<inat::PhotoIdentifier> m_identifier;
};

void TestRunSuggester::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Lib"));
    const QString folder = m_root + QStringLiteral("/Unsorted 2016");
    QDir().mkpath(folder);
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::darkGreen);
    for (const char *name : {"DSC_0001", "DSC_0002", "DSC_0003", "DSC_0004"})
        QVERIFY(img.save(folder + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".jpg")));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());

    // Two shots 10 s apart, one five minutes later, one with only a day.
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2016-03-12T10:00:00' WHERE base_name = 'DSC_0001'")));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2016-03-12T10:00:10' WHERE base_name = 'DSC_0002'")));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2016-03-12T10:05:00' WHERE base_name = 'DSC_0003'")));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2016-03-12' WHERE base_name = 'DSC_0004'")));

    taxonomy::TaxonomyStore store(m_db->connectionName());
    taxonomy::Taxon robin;
    robin.inatId = 12936;
    robin.rank = QStringLiteral("species");
    robin.name = QStringLiteral("Petroica phoenicea");
    robin.commonName = QStringLiteral("Flame Robin");
    store.upsertTaxon(robin);

    match::MatchEngine(m_db->connectionName()).matchAll();   // nothing to go on: no guess
}

void TestRunSuggester::cleanup()
{
    m_identifier.reset();
    m_inat.reset();
    m_http.reset();
    m_db.reset();
}

void TestRunSuggester::wire(std::function<Transport::Reply(const Transport::Request &, int)> responder)
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
    m_inat->setAccessToken(QStringLiteral("eyJ.token"));
    m_identifier = std::make_unique<inat::PhotoIdentifier>(*m_inat, m_db->connectionName());
}

qint64 TestRunSuggester::captureId(const QString &base) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT id FROM capture WHERE base_name = ?"));
    q.addBindValue(base);
    return q.exec() && q.next() ? q.value(0).toLongLong() : -1;
}

QList<qint64> TestRunSuggester::allCaptures() const
{
    QList<qint64> ids;
    for (const char *name : {"DSC_0001", "DSC_0002", "DSC_0003", "DSC_0004"})
        ids << captureId(QLatin1String(name));
    return ids;
}

int TestRunSuggester::scoreRequests() const
{
    int n = 0;
    for (const Transport::Request &r : m_transport->received)
        n += r.url.path().endsWith(QStringLiteral("score_image")) ? 1 : 0;
    return n;
}

qint64 TestRunSuggester::suggestionInat(qint64 cid) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT t.inat_id FROM capture_match m JOIN taxon t ON t.id = m.taxon_id "
                             "WHERE m.capture_id = ? AND m.decided_by = 'vision' AND m.status = 'pending'"));
    q.addBindValue(cid);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

RunSuggester::Result TestRunSuggester::runAll()
{
    RunSuggester suggester(*m_inat, *m_identifier, m_db->connectionName());
    QSignalSpy done(&suggester, &RunSuggester::finished);
    suggester.start(allCaptures());
    if (!done.wait(10000))
        return {};
    return done.first().first().value<RunSuggester::Result>();
}

void TestRunSuggester::splitsIntoRunsByTheGapBetweenShots()
{
    const QList<RunSuggester::Photo> photos = RunSuggester::loadPhotos(m_db->connectionName(), allCaptures());
    QCOMPARE(photos.size(), 4);
    const auto runs = RunSuggester::splitRuns(photos, 30);
    QCOMPARE(runs.size(), 3);
    QCOMPARE(runs.at(0).size(), 2);   // 10:00:00 and 10:00:10
    QCOMPARE(runs.at(1).size(), 1);   // 10:05
    QCOMPARE(runs.at(2).size(), 1);   // the day only: its own run
    QVERIFY(!runs.at(2).first().taken.isValid());
    QCOMPARE(RunSuggester::splitRuns(photos, 600).size(), 2);   // a wider gap joins 10:05 on
}

void TestRunSuggester::asksOncePerRunAndGivesEveryPhotoTheSuggestion()
{
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kFlameRobin); });
    const RunSuggester::Result r = runAll();
    QCOMPARE(r.runs, 3);
    QCOMPARE(r.photos, 4);
    QCOMPARE(r.failed, 0);
    QCOMPARE(scoreRequests(), 3);   // one per run, not per photo
    for (qint64 cid : allCaptures())
        QCOMPARE(suggestionInat(cid), qint64(12936));

    // In the review queue: a guess now, and marked as iNaturalist's.
    thumb::ThumbnailCache thumbs(m_tmp.filePath(QStringLiteral("thumbs")));
    model::ReviewQueueModel queue(*m_db, thumbs);
    queue.reload();
    QCOMPARE(queue.rowCount(), 4);
    QCOMPARE(queue.noCandidateCount(), 0);
    const QModelIndex first = queue.index(0);
    QCOMPARE(first.data(model::ReviewQueueModel::GuessNameRole).toString(), QStringLiteral("Petroica phoenicea"));
    QVERIFY(first.data(model::ReviewQueueModel::SuggestedRole).toBool());
    QCOMPARE(qRound(first.data(model::ReviewQueueModel::ConfidenceRole).toDouble() * 100), 91);
}

void TestRunSuggester::matchLibraryKeepsTheSuggestion()
{
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kFlameRobin); });
    QCOMPARE(runAll().photos, 4);
    match::MatchEngine(m_db->connectionName()).matchAll();
    for (qint64 cid : allCaptures())
        QCOMPARE(suggestionInat(cid), qint64(12936));
}

void TestRunSuggester::confirmingASuggestionLearnsNoAlias()
{
    wire([](const Transport::Request &, int) { return FakeTransport::ok(kFlameRobin); });
    QCOMPARE(runAll().photos, 4);
    const qint64 cid = captureId(QStringLiteral("DSC_0001"));
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY2(reviewer.confirm(cid, 12936), qPrintable(reviewer.error()));
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM name_alias")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 0);   // "Unsorted 2016" isn't a name for a Flame Robin
    QVERIFY(q.exec(QStringLiteral("SELECT status, decided_by FROM capture_match WHERE capture_id = %1").arg(cid)));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("confirmed"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("user"));
    QVERIFY(!q.next());   // one live decision
}

void TestRunSuggester::anExpiredTokenStopsTheRun()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(401); });
    const RunSuggester::Result r = runAll();
    QCOMPARE(r.status, 401);
    QCOMPARE(r.photos, 0);
    QCOMPARE(scoreRequests(), 1);   // stopped at the first
    for (qint64 cid : allCaptures())
        QCOMPARE(suggestionInat(cid), qint64(0));
}

QTEST_MAIN(TestRunSuggester)
#include "tst_runsuggester.moc"
