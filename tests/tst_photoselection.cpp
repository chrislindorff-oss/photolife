#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "db/Database.h"
#include "libsync/PhotoSelection.h"

using namespace pl;
using namespace pl::libsync;

// Which photos stay online only when some reference trees are ticked.
class TestPhotoSelection : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void onlyTickedTreesPhotosAreKept();
    void unmatchedAndPendingPhotosAreAlwaysKept();
    void photosMatchedOutsideEveryTreeAreKept();
    void aPhotoInATickedAndAnUntickedTreeIsKept();
    void rawAndJpegGoTogether();
    void nothingTickedLeavesEveryTreesPhotosOnline();
    void treeStatsCountPhotosAndBytes();

private:
    std::unique_ptr<Database> m_db;
    int m_orchids = 0;
    int m_birds = 0;
    int m_caladenia = 0;
    int m_robin = 0;
    int m_untreed = 0;

    QSqlQuery query() const { return QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false)); }
    void exec(const QString &sql)
    {
        QSqlQuery q = query();
        QVERIFY2(q.exec(sql), qPrintable(sql));
    }
    int capture(const QString &name, const QStringList &exts = {QStringLiteral("jpg")})
    {
        QSqlQuery q = query();
        q.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (1, ?)"));
        q.addBindValue(name);
        q.exec();
        const int id = q.lastInsertId().toInt();
        for (const QString &ext : exts) {
            QSqlQuery r = query();
            r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash, "
                                     "file_size) VALUES (?, ?, ?, ?, ?, 100)"));
            r.addBindValue(id);
            r.addBindValue(QStringLiteral("/lib/%1.%2").arg(name, ext));
            r.addBindValue(ext == QLatin1String("jpg") ? QStringLiteral("jpeg") : QStringLiteral("raw"));
            r.addBindValue(ext);
            r.addBindValue(QStringLiteral("hash-%1-%2").arg(name, ext));
            r.exec();
        }
        return id;
    }
    void match(int captureId, int taxonId, const QString &status = QStringLiteral("confirmed"))
    {
        exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, status) "
                            "VALUES (%1, %2, 'test', '%3')")
                 .arg(captureId)
                 .arg(taxonId)
                 .arg(status));
    }
    QSet<QString> notWanted(const QList<int> &ticked)
    {
        QString error;
        const auto out = PhotoSelection::notWanted(m_db->connectionName(), ticked, &error);
        if (!out)
            qWarning() << error;
        return out.value_or(QSet<QString>{QStringLiteral("<error>")});
    }
};

void TestPhotoSelection::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)"));
    exec(QStringLiteral("INSERT INTO taxon (id, inat_id, rank, rank_level, name) VALUES "
                        "(1, 101, 'species', 10, 'Caladenia latifolia'),"
                        "(2, 102, 'species', 10, 'Petroica boodang'),"
                        "(3, 103, 'species', 10, 'Untreed thing')"));
    m_caladenia = 1;
    m_robin = 2;
    m_untreed = 3;
    exec(QStringLiteral("INSERT INTO project (id, name) VALUES (1, 'Orchids'), (2, 'Birds')"));
    m_orchids = 1;
    m_birds = 2;
    exec(QStringLiteral("INSERT INTO project_taxon (project_id, taxon_id) VALUES (1, 1), (2, 2)"));
}

void TestPhotoSelection::cleanup()
{
    m_db.reset();
}

void TestPhotoSelection::onlyTickedTreesPhotosAreKept()
{
    match(capture(QStringLiteral("orchid")), m_caladenia);
    match(capture(QStringLiteral("robin")), m_robin);
    QCOMPARE(notWanted({m_orchids}), QSet<QString>{QStringLiteral("/lib/robin.jpg")});
    QCOMPARE(notWanted({m_birds}), QSet<QString>{QStringLiteral("/lib/orchid.jpg")});
    QVERIFY(notWanted({m_orchids, m_birds}).isEmpty());
}

void TestPhotoSelection::unmatchedAndPendingPhotosAreAlwaysKept()
{
    capture(QStringLiteral("unmatched"));
    match(capture(QStringLiteral("pending")), m_robin, QStringLiteral("pending"));
    match(capture(QStringLiteral("rejected")), m_robin, QStringLiteral("rejected"));
    QVERIFY(notWanted({m_orchids}).isEmpty());
}

void TestPhotoSelection::photosMatchedOutsideEveryTreeAreKept()
{
    match(capture(QStringLiteral("stray")), m_untreed);
    QVERIFY(notWanted({m_orchids}).isEmpty());
}

void TestPhotoSelection::aPhotoInATickedAndAnUntickedTreeIsKept()
{
    const int both = capture(QStringLiteral("both"));
    match(both, m_caladenia);
    match(both, m_robin, QStringLiteral("auto"));
    QVERIFY(notWanted({m_orchids}).isEmpty());
    QVERIFY(notWanted({m_birds}).isEmpty());
}

void TestPhotoSelection::rawAndJpegGoTogether()
{
    match(capture(QStringLiteral("robin"), {QStringLiteral("jpg"), QStringLiteral("nef")}), m_robin);
    QCOMPARE(notWanted({m_orchids}),
             (QSet<QString>{QStringLiteral("/lib/robin.jpg"), QStringLiteral("/lib/robin.nef")}));
}

void TestPhotoSelection::nothingTickedLeavesEveryTreesPhotosOnline()
{
    match(capture(QStringLiteral("orchid")), m_caladenia);
    match(capture(QStringLiteral("robin")), m_robin);
    capture(QStringLiteral("unmatched"));
    QCOMPARE(notWanted({}),
             (QSet<QString>{QStringLiteral("/lib/orchid.jpg"), QStringLiteral("/lib/robin.jpg")}));
}

void TestPhotoSelection::treeStatsCountPhotosAndBytes()
{
    match(capture(QStringLiteral("robin"), {QStringLiteral("jpg"), QStringLiteral("nef")}), m_robin);
    match(capture(QStringLiteral("robin2")), m_robin);
    const auto stats = PhotoSelection::treeStats(m_db->connectionName());
    QCOMPARE(stats.size(), 2);
    QCOMPARE(stats.at(0).name, QStringLiteral("Birds"));
    QCOMPARE(stats.at(0).photos, 2);
    QCOMPARE(stats.at(0).bytes, qint64(300));
    QCOMPARE(stats.at(1).name, QStringLiteral("Orchids"));
    QCOMPARE(stats.at(1).photos, 0);
    QCOMPARE(PhotoSelection::contentHashes(m_db->connectionName()).value(QStringLiteral("/lib/robin.nef")),
             QStringLiteral("hash-robin-nef"));
}

QTEST_GUILESS_MAIN(TestPhotoSelection)
#include "tst_photoselection.moc"
