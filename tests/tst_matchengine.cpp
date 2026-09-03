#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "match/MatchEngine.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::match;

namespace {
void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("x");
}
} // namespace

class TestMatchEngine : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void autoMatchesCleanBinomial();
    void reconcilesSynonymAgainstFolder();
    void matchesAtGenusForSpQualifier();
    void queuesFuzzyTypo();
    void stagingFolderIsSkipped();
    void unmatchedWhenTaxonomyMissing();
    void reRunReplacesEngineRowsButKeepsUserRows();

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;

    void seedTaxonomy();
    QSqlQuery matchRow(const QString &baseNameLike);
};

void TestMatchEngine::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Library"));

    touch(m_root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Caladenia carnea - Anglesea 1-1-2020.jpg"));
    touch(m_root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Petalochilus carneus - Anglesea 2-1-2020.jpg"));
    touch(m_root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Caladenia/Caladenia sp/"
                                  "Caladenia sp - Brisbane Ranges 3-1-2020.jpg"));
    touch(m_root + QStringLiteral("/4. DICOTYLEDONS/Asparagaceae/Asparagus/Asaparagus scandens/"
                                  "Asaparagus scandens - Otways 4-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Duplicates/mystery plant - Somewhere 5-1-2020.jpg"));
    touch(m_root + QStringLiteral("/3. MONOCOTYLEDONS/Orchidaceae/Thelymitra/Thelymitra ixioides/"
                                  "Thelymitra ixioides - Grampians 6-1-2020.jpg"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));

    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());

    seedTaxonomy();
}

void TestMatchEngine::cleanup()
{
    m_db.reset();
}

void TestMatchEngine::seedTaxonomy()
{
    taxonomy::TaxonomyStore store(m_db->connectionName());

    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name,
                   const QStringList &syn = {}) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        t.name = name;
        t.synonyms = syn;
        store.upsertTaxon(t);
    };

    add(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    add(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    add(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"),
        {QStringLiteral("Petalochilus carneus")});
    add(110, 1, QStringLiteral("genus"), QStringLiteral("Thelymitra"));
    add(111, 110, QStringLiteral("species"), QStringLiteral("Thelymitra ixioides"));
    add(300, 0, QStringLiteral("genus"), QStringLiteral("Asparagus"));
    add(301, 300, QStringLiteral("species"), QStringLiteral("Asparagus scandens"));
}

QSqlQuery TestMatchEngine::matchRow(const QString &baseNameLike)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT m.taxon_id, m.matched_rank, m.method, m.confidence, m.status, m.qualifier, m.note "
        "FROM capture_match m JOIN capture c ON c.id = m.capture_id "
        "WHERE c.base_name LIKE ?"));
    q.addBindValue(baseNameLike);
    q.exec();
    q.next();
    return q;
}

void TestMatchEngine::autoMatchesCleanBinomial()
{
    MatchEngine engine(m_db->connectionName());
    const auto stats = engine.matchAll();
    QVERIFY2(stats.ok(), qPrintable(stats.error));
    QCOMPARE(stats.captures, 6);
    QVERIFY(stats.autoApplied >= 3);

    auto q = matchRow(QStringLiteral("Thelymitra ixioides%"));
    QCOMPARE(q.value(4).toString(), QStringLiteral("auto"));
    QCOMPARE(q.value(2).toString(), QStringLiteral("folder+file"));
    QVERIFY(q.value(3).toDouble() >= 0.9);
    // taxon 111 -> local id
    QSqlQuery loc(QSqlDatabase::database(m_db->connectionName(), false));
    loc.exec(QStringLiteral("SELECT id FROM taxon WHERE inat_id = 111"));
    loc.next();
    QCOMPARE(q.value(0).toInt(), loc.value(0).toInt());
}

void TestMatchEngine::reconcilesSynonymAgainstFolder()
{
    MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    auto q = matchRow(QStringLiteral("Petalochilus carneus%"));
    QSqlQuery loc(QSqlDatabase::database(m_db->connectionName(), false));
    loc.exec(QStringLiteral("SELECT id FROM taxon WHERE inat_id = 101"));
    loc.next();
    QCOMPARE(q.value(0).toInt(), loc.value(0).toInt());   // resolved to Caladenia carnea
    QCOMPARE(q.value(4).toString(), QStringLiteral("auto"));
}

void TestMatchEngine::matchesAtGenusForSpQualifier()
{
    MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    auto q = matchRow(QStringLiteral("Caladenia sp %"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("genus"));
    QCOMPARE(q.value(5).toString(), QStringLiteral("sp"));
    QSqlQuery loc(QSqlDatabase::database(m_db->connectionName(), false));
    loc.exec(QStringLiteral("SELECT id FROM taxon WHERE inat_id = 100"));
    loc.next();
    QCOMPARE(q.value(0).toInt(), loc.value(0).toInt());
}

void TestMatchEngine::queuesFuzzyTypo()
{
    MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    auto q = matchRow(QStringLiteral("Asaparagus scandens%"));
    QSqlQuery loc(QSqlDatabase::database(m_db->connectionName(), false));
    loc.exec(QStringLiteral("SELECT id FROM taxon WHERE inat_id = 301"));
    loc.next();
    QCOMPARE(q.value(0).toInt(), loc.value(0).toInt());
    QCOMPARE(q.value(4).toString(), QStringLiteral("pending"));
    QVERIFY(q.value(3).toDouble() < 0.9);
}

void TestMatchEngine::stagingFolderIsSkipped()
{
    MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    auto q = matchRow(QStringLiteral("mystery plant%"));
    QVERIFY(q.value(0).isNull());
    QCOMPARE(q.value(4).toString(), QStringLiteral("pending"));
    QVERIFY(q.value(6).toString().contains(QStringLiteral("staging")));
}

void TestMatchEngine::unmatchedWhenTaxonomyMissing()
{
    // Wipe the taxonomy so nothing resolves.
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("DELETE FROM taxon"));

    MatchEngine engine(m_db->connectionName());
    const auto stats = engine.matchAll();
    QCOMPARE(stats.autoApplied, 0);
    QVERIFY(stats.unmatched >= 4);
}

void TestMatchEngine::reRunReplacesEngineRowsButKeepsUserRows()
{
    MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    // A user confirms one capture manually.
    QSqlQuery upd(QSqlDatabase::database(m_db->connectionName(), false));
    upd.exec(QStringLiteral(
        "UPDATE capture_match SET decided_by = 'user', status = 'confirmed' "
        "WHERE capture_id = (SELECT c.id FROM capture c WHERE c.base_name LIKE 'Caladenia carnea%')"));

    const int before = [&] {
        QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
        q.exec(QStringLiteral("SELECT COUNT(*) FROM capture_match WHERE decided_by = 'user'"));
        q.next();
        return q.value(0).toInt();
    }();
    QCOMPARE(before, 1);

    engine.matchAll();   // re-run

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM capture_match WHERE decided_by = 'user' "
                          "AND status = 'confirmed'"));
    q.next();
    QCOMPARE(q.value(0).toInt(), 1);   // user's decision survived the re-run
}

QTEST_GUILESS_MAIN(TestMatchEngine)
#include "tst_matchengine.moc"
