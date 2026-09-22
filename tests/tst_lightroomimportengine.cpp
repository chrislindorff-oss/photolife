#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "lightroom/LightroomImportEngine.h"
#include "match/MatchEngine.h"
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
} // namespace

class TestLightroomImportEngine : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void resolvesKeywordAndWritesHint();
    void reimportIsIdempotent();
    void unresolvedPhotoPathIsSkipped();
    void noiseKeywordIsNotStored();
    void hintBecomesSoleSourceForMatchEngine();

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;
    QString m_photoPath;   // exactly as stored in rendition.path

    int scalar(const QString &sql);
};

void TestLightroomImportEngine::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Library"));

    // A capture whose filename and folder resolve to nothing: no scientific
    // name in the filename, and a folder that classifies as "unknown" (not a
    // taxon, staging, or locality folder) -- so the only source MatchEngine
    // can use for it is a keyword hint.
    touch(m_root + QStringLiteral("/2020-01-01/IMG_0001.jpg"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));

    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("SELECT path FROM rendition")));
    QVERIFY(q.next());
    m_photoPath = q.value(0).toString();

    taxonomy::TaxonomyStore store(m_db->connectionName());
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
}

void TestLightroomImportEngine::cleanup()
{
    m_db.reset();
}

int TestLightroomImportEngine::scalar(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

void TestLightroomImportEngine::resolvesKeywordAndWritesHint()
{
    LightroomPhotoKeywords photo;
    photo.absolutePath = m_photoPath;
    photo.keywords = {QStringLiteral("Diuris pardina")};

    LightroomImportEngine engine(m_db->connectionName());
    const auto stats = engine.import({photo});

    QVERIFY(stats.ok());
    QCOMPARE(stats.photosMatched, 1);
    QCOMPARE(stats.photosUnmatched, 0);
    QCOMPARE(stats.keywordsResolved, 1);
    QCOMPARE(stats.hintsWritten, 1);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral(
        "SELECT ckh.raw_keyword, ckh.confidence, t.inat_id FROM capture_keyword_hint ckh "
        "JOIN taxon t ON t.id = ckh.taxon_id")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("Diuris pardina"));
    QCOMPARE(q.value(1).toDouble(), 1.0);
    QCOMPARE(q.value(2).toLongLong(), qlonglong(101));
}

void TestLightroomImportEngine::reimportIsIdempotent()
{
    LightroomPhotoKeywords photo;
    photo.absolutePath = m_photoPath;
    photo.keywords = {QStringLiteral("Diuris pardina")};

    LightroomImportEngine engine(m_db->connectionName());
    QVERIFY(engine.import({photo}).ok());
    const auto second = engine.import({photo});

    QVERIFY(second.ok());
    QCOMPARE(second.hintsWritten, 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM capture_keyword_hint")), 1);
}

void TestLightroomImportEngine::unresolvedPhotoPathIsSkipped()
{
    LightroomPhotoKeywords photo;
    photo.absolutePath = QStringLiteral("/nowhere/on/disk.jpg");
    photo.keywords = {QStringLiteral("Diuris pardina")};

    LightroomImportEngine engine(m_db->connectionName());
    const auto stats = engine.import({photo});

    QVERIFY(stats.ok());
    QCOMPARE(stats.photosMatched, 0);
    QCOMPARE(stats.photosUnmatched, 1);
    QCOMPARE(stats.hintsWritten, 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM capture_keyword_hint")), 0);
}

void TestLightroomImportEngine::noiseKeywordIsNotStored()
{
    LightroomPhotoKeywords photo;
    photo.absolutePath = m_photoPath;
    // A locality/event tag that the resolver won't match to any taxon.
    photo.keywords = {QStringLiteral("Anglesea Heathland Walk")};

    LightroomImportEngine engine(m_db->connectionName());
    const auto stats = engine.import({photo});

    QVERIFY(stats.ok());
    QCOMPARE(stats.photosMatched, 1);
    QCOMPARE(stats.keywordsResolved, 0);
    QCOMPARE(stats.hintsWritten, 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM capture_keyword_hint")), 0);
}

void TestLightroomImportEngine::hintBecomesSoleSourceForMatchEngine()
{
    LightroomPhotoKeywords photo;
    photo.absolutePath = m_photoPath;
    photo.keywords = {QStringLiteral("Diuris pardina")};

    LightroomImportEngine importEngine(m_db->connectionName());
    QVERIFY(importEngine.import({photo}).ok());

    match::MatchEngine matcher(m_db->connectionName());
    const auto stats = matcher.matchAll();
    QVERIFY(stats.ok());
    QCOMPARE(stats.autoApplied, 1);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral(
        "SELECT cm.status, cm.method, t.inat_id FROM capture_match cm "
        "JOIN taxon t ON t.id = cm.taxon_id")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("auto"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("keyword"));
    QCOMPARE(q.value(2).toLongLong(), qlonglong(101));
}

QTEST_GUILESS_MAIN(TestLightroomImportEngine)
#include "tst_lightroomimportengine.moc"
