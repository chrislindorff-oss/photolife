#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "match/MatchEngine.h"
#include "match/MatchReviewer.h"
#include "match/TaxonResolver.h"
#include "match/NameParser.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"

#include "PgTestDsn.h"

using namespace pl;
using namespace pl::match;

class TestMatchReviewer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void confirmLearnsAliasAndSurvivesReMatch();
    void rejectDropsFromQueue();
    void reassignChangesTaxon();
    void reassigningAConfirmedCaptureDropsTheOldTaxon();
    void markNotATaxonScopesToFolderAndReclassifies();
    void applyTaxonToFolderBulkConfirms();
    void ignoreFolderTreeStagesEverything();
    void pgReassignLeavesExactlyOneLiveDecision();

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;

    qint64 captureId(const QString &likeBase);
    QString matchStatus(qint64 captureId);
    qint64 matchTaxonInat(qint64 captureId);
};

void TestMatchReviewer::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Lib"));

    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Caladenia carnea - Loc 1-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Petalochilus mystery - Loc 2-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Thompson Track BRNP/x - Loc 3-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Orchidaceae/Diuris/Diuris sp/Diuris sp - Loc 4-1-2020.jpg"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));

    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());

    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        t.name = name;
        store.upsertTaxon(t);
    };
    add(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    add(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    add(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    add(102, 100, QStringLiteral("species"), QStringLiteral("Caladenia fuscata"));
    add(200, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));

    MatchEngine(m_db->connectionName()).matchAll();
}

void TestMatchReviewer::cleanup()
{
    m_db.reset();
}

qint64 TestMatchReviewer::captureId(const QString &likeBase)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT id FROM capture WHERE base_name LIKE ?"));
    q.addBindValue(likeBase);
    q.exec();
    return q.next() ? q.value(0).toLongLong() : -1;
}

QString TestMatchReviewer::matchStatus(qint64 cid)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT status FROM capture_match WHERE capture_id = ? "
                             "ORDER BY (decided_by='user') DESC LIMIT 1"));
    q.addBindValue(qlonglong(cid));
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

qint64 TestMatchReviewer::matchTaxonInat(qint64 cid)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT t.inat_id FROM capture_match m JOIN taxon t ON t.id = m.taxon_id "
        "WHERE m.capture_id = ? ORDER BY (m.decided_by='user') DESC LIMIT 1"));
    q.addBindValue(qlonglong(cid));
    q.exec();
    return q.next() ? q.value(0).toLongLong() : -1;
}

void TestMatchReviewer::confirmLearnsAliasAndSurvivesReMatch()
{
    const qint64 cid = captureId(QStringLiteral("Petalochilus mystery%"));
    QVERIFY(cid > 0);

    MatchReviewer reviewer(m_db->connectionName());
    QVERIFY2(reviewer.confirm(cid, 101), qPrintable(reviewer.error()));

    QCOMPARE(matchStatus(cid), QStringLiteral("confirmed"));
    QCOMPARE(matchTaxonInat(cid), qint64(101));

    // An alias was learned for the odd name.
    QSqlQuery a(QSqlDatabase::database(m_db->connectionName(), false));
    a.exec(QStringLiteral("SELECT t.inat_id FROM name_alias na JOIN taxon t ON t.id = na.taxon_id "
                          "WHERE na.raw_folded = 'petalochilus mystery'"));
    QVERIFY(a.next());
    QCOMPARE(a.value(0).toLongLong(), qint64(101));

    // Re-matching does not clobber the user's decision.
    MatchEngine(m_db->connectionName()).matchAll();
    QCOMPARE(matchStatus(cid), QStringLiteral("confirmed"));
    QCOMPARE(matchTaxonInat(cid), qint64(101));
}

void TestMatchReviewer::rejectDropsFromQueue()
{
    const qint64 cid = captureId(QStringLiteral("Petalochilus mystery%"));
    MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.reject(cid));
    QCOMPARE(matchStatus(cid), QStringLiteral("rejected"));

    MatchEngine(m_db->connectionName()).matchAll();
    QCOMPARE(matchStatus(cid), QStringLiteral("rejected"));
}

void TestMatchReviewer::reassignChangesTaxon()
{
    const qint64 cid = captureId(QStringLiteral("Caladenia carnea - Loc%"));
    MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(cid, 102));   // reassign carnea -> fuscata
    QCOMPARE(matchTaxonInat(cid), qint64(102));
    QCOMPARE(matchStatus(cid), QStringLiteral("confirmed"));
}

void TestMatchReviewer::reassigningAConfirmedCaptureDropsTheOldTaxon()
{
    const qint64 cid = captureId(QStringLiteral("Caladenia carnea - Loc%"));
    MatchReviewer reviewer(m_db->connectionName());

    // First confirm to carnea (101) — this is now a *user* decision, unlike the
    // engine's original guess. Reassigning away from it must not leave it behind.
    QVERIFY(reviewer.confirm(cid, 101));
    QCOMPARE(matchTaxonInat(cid), qint64(101));

    QVERIFY(reviewer.confirm(cid, 102));   // reassign carnea -> fuscata, again
    QCOMPARE(matchTaxonInat(cid), qint64(102));
    QCOMPARE(matchStatus(cid), QStringLiteral("confirmed"));

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM capture_match WHERE capture_id = ?"));
    q.addBindValue(qlonglong(cid));
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 1);   // exactly one live row — the old taxon is gone
}

void TestMatchReviewer::markNotATaxonScopesToFolderAndReclassifies()
{
    const qint64 cid = captureId(QStringLiteral("x - Loc 3-1-2020%"));
    QVERIFY(cid > 0);

    MatchReviewer reviewer(m_db->connectionName());
    QVERIFY2(reviewer.markNotATaxon(cid, true), qPrintable(reviewer.error()));

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT kind FROM folder WHERE name = 'Thompson Track BRNP'"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("locality"));

    q.exec(QStringLiteral("SELECT not_a_taxon, scope FROM name_alias "
                          "WHERE raw_folded = 'thompson track brnp'"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
    QVERIFY(q.value(1).toString().contains(QStringLiteral("Thompson Track BRNP")));
}

void TestMatchReviewer::applyTaxonToFolderBulkConfirms()
{
    QSqlQuery f(QSqlDatabase::database(m_db->connectionName(), false));
    f.exec(QStringLiteral("SELECT id FROM folder WHERE name = 'Caladenia carnea'"));
    QVERIFY(f.next());
    const int folderId = f.value(0).toInt();

    MatchReviewer reviewer(m_db->connectionName());
    const int n = reviewer.applyTaxonToFolder(folderId, 101, false, true);
    QVERIFY(n >= 1);

    // The "Petalochilus mystery" capture (was pending) is now confirmed as 101.
    const qint64 cid = captureId(QStringLiteral("Petalochilus mystery%"));
    QCOMPARE(matchStatus(cid), QStringLiteral("confirmed"));
    QCOMPARE(matchTaxonInat(cid), qint64(101));
}

void TestMatchReviewer::ignoreFolderTreeStagesEverything()
{
    QSqlQuery f(QSqlDatabase::database(m_db->connectionName(), false));
    f.exec(QStringLiteral("SELECT id FROM folder WHERE name = 'Diuris'"));
    QVERIFY(f.next());
    const int folderId = f.value(0).toInt();

    MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.ignoreFolderTree(folderId) >= 2);   // Diuris + Diuris sp

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM folder WHERE name IN ('Diuris','Diuris sp') "
                          "AND kind = 'staging'"));
    q.next();
    QCOMPARE(q.value(0).toInt(), 2);
}

void TestMatchReviewer::pgReassignLeavesExactlyOneLiveDecision()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed MatchReviewer tests");

    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));

    QSqlDatabase conn = QSqlDatabase::database(db.connectionName(), false);
    // This DSN is expected to point at a scratch database dedicated to
    // PhotoLife's test suite -- clear it so repeated local runs don't pile
    // up rows from earlier ones.
    QVERIFY(conn.exec(QStringLiteral(
                              "TRUNCATE folder, capture, taxon, capture_match "
                              "RESTART IDENTITY CASCADE"))
                .lastError()
                .type()
            == QSqlError::NoError);

    taxonomy::TaxonomyStore store(db.connectionName());
    auto addTaxon = [&](qint64 id, const QString &rank, const QString &name) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.rank = rank;
        t.name = name;
        QVERIFY(store.upsertTaxon(t) > 0);
    };
    addTaxon(101, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    addTaxon(102, QStringLiteral("species"), QStringLiteral("Caladenia fuscata"));

    QSqlQuery folderIns(conn);
    folderIns.prepare(QStringLiteral(
        "INSERT INTO folder (path, name, depth) VALUES (?, ?, 0) RETURNING id"));
    folderIns.addBindValue(QStringLiteral("/pg-scratch"));
    folderIns.addBindValue(QStringLiteral("pg-scratch"));
    QVERIFY2(folderIns.exec(), qPrintable(folderIns.lastError().text()));
    QVERIFY(folderIns.next());
    const int folderId = folderIns.value(0).toInt();

    QSqlQuery captureIns(conn);
    captureIns.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name) VALUES (?, ?) RETURNING id"));
    captureIns.addBindValue(folderId);
    captureIns.addBindValue(QStringLiteral("Caladenia carnea - Loc 1-1-2020"));
    QVERIFY2(captureIns.exec(), qPrintable(captureIns.lastError().text()));
    QVERIFY(captureIns.next());
    const qint64 cid = captureIns.value(0).toLongLong();

    MatchReviewer reviewer(db.connectionName());
    QVERIFY2(reviewer.confirm(cid, 101, false), qPrintable(reviewer.error()));
    QVERIFY2(reviewer.confirm(cid, 102, false), qPrintable(reviewer.error()));

    QSqlQuery count(conn);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM capture_match WHERE capture_id = ?"));
    count.addBindValue(cid);
    QVERIFY(count.exec());
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 1);

    QSqlQuery taxon(conn);
    taxon.prepare(QStringLiteral(
        "SELECT t.inat_id FROM capture_match m JOIN taxon t ON t.id = m.taxon_id "
        "WHERE m.capture_id = ?"));
    taxon.addBindValue(cid);
    QVERIFY(taxon.exec());
    QVERIFY(taxon.next());
    QCOMPARE(taxon.value(0).toLongLong(), qint64(102));
}

QTEST_GUILESS_MAIN(TestMatchReviewer)
#include "tst_matchreviewer.moc"
