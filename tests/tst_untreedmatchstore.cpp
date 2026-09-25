#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "catalogue/UntreedMatchStore.h"
#include "db/Database.h"

using namespace pl;
using namespace pl::catalogue;

class TestUntreedMatchStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void listsAutoAndConfirmedMatchesInNoTree();
    void taxonInAnyTreeIsExcluded();
    void selectedTreeModeListsTaxaFromOtherTrees();
    void pendingRejectedAndUnmatchedAreExcluded();
    void onlyTheTopMatchPerCaptureCounts();
    void genusMatchCarriesItsRank();
    void aboveSpeciesListsGenusMatchesEvenInATree();

private:
    std::unique_ptr<Database> m_db;

    QSqlQuery query() const;
    int seedTaxon(qint64 inatId, const QString &name, const QString &rank, int rankLevel);
    int seedCapture(const QString &baseName);
    void seedMatch(int captureId, int taxonId, const QString &status, double confidence = 0.9,
                   const QString &decidedBy = QStringLiteral("engine"));
    int seedProject(const QString &name);
    void linkTaxon(int projectId, int taxonId);
    static const UntreedTaxon *find(const QList<UntreedTaxon> &list, qint64 inatId);
};

QSqlQuery TestUntreedMatchStore::query() const
{
    return QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false));
}

void TestUntreedMatchStore::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    QVERIFY(query().exec(QStringLiteral(
        "INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)")));
}

void TestUntreedMatchStore::cleanup()
{
    m_db.reset();
}

int TestUntreedMatchStore::seedTaxon(qint64 inatId, const QString &name, const QString &rank,
                                     int rankLevel)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral(
        "INSERT INTO taxon (inat_id, rank, rank_level, name) VALUES (?, ?, ?, ?)"));
    q.addBindValue(inatId);
    q.addBindValue(rank);
    q.addBindValue(rankLevel);
    q.addBindValue(name);
    q.exec();
    return q.lastInsertId().toInt();
}

int TestUntreedMatchStore::seedCapture(const QString &baseName)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (1, ?)"));
    q.addBindValue(baseName);
    q.exec();
    return q.lastInsertId().toInt();
}

void TestUntreedMatchStore::seedMatch(int captureId, int taxonId, const QString &status,
                                      double confidence, const QString &decidedBy)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
        "VALUES (?, ?, 'exact', ?, ?, ?)"));
    q.addBindValue(captureId);
    q.addBindValue(taxonId > 0 ? QVariant(taxonId) : QVariant());
    q.addBindValue(confidence);
    q.addBindValue(status);
    q.addBindValue(decidedBy);
    QVERIFY(q.exec());
}

int TestUntreedMatchStore::seedProject(const QString &name)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO project (name) VALUES (?)"));
    q.addBindValue(name);
    q.exec();
    return q.lastInsertId().toInt();
}

void TestUntreedMatchStore::linkTaxon(int projectId, int taxonId)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO project_taxon (project_id, taxon_id) VALUES (?, ?)"));
    q.addBindValue(projectId);
    q.addBindValue(taxonId);
    QVERIFY(q.exec());
}

const UntreedTaxon *TestUntreedMatchStore::find(const QList<UntreedTaxon> &list, qint64 inatId)
{
    for (const UntreedTaxon &t : list)
        if (t.inatId == inatId)
            return &t;
    return nullptr;
}

void TestUntreedMatchStore::listsAutoAndConfirmedMatchesInNoTree()
{
    const int taxon = seedTaxon(100, QStringLiteral("Litoria aurea"), QStringLiteral("species"), 10);
    seedMatch(seedCapture(QStringLiteral("a")), taxon, QStringLiteral("auto"));
    seedMatch(seedCapture(QStringLiteral("b")), taxon, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("c")), taxon, QStringLiteral("confirmed"));

    UntreedMatchStore store(m_db->connectionName());
    const QList<UntreedTaxon> list = store.untreedTaxa();
    QVERIFY2(store.error().isEmpty(), qPrintable(store.error()));
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].inatId, qint64(100));
    QCOMPARE(list[0].name, QStringLiteral("Litoria aurea"));
    QCOMPARE(list[0].photoCount, 3);
    QCOMPARE(list[0].confirmedCount, 2);
}

void TestUntreedMatchStore::taxonInAnyTreeIsExcluded()
{
    const int inTree = seedTaxon(100, QStringLiteral("In tree"), QStringLiteral("species"), 10);
    const int outside = seedTaxon(200, QStringLiteral("Outside"), QStringLiteral("species"), 10);
    seedMatch(seedCapture(QStringLiteral("a")), inTree, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("b")), outside, QStringLiteral("confirmed"));
    linkTaxon(seedProject(QStringLiteral("Frogs")), inTree);

    const QList<UntreedTaxon> list = UntreedMatchStore(m_db->connectionName()).untreedTaxa();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].inatId, qint64(200));
}

void TestUntreedMatchStore::selectedTreeModeListsTaxaFromOtherTrees()
{
    const int frog = seedTaxon(100, QStringLiteral("Frog"), QStringLiteral("species"), 10);
    const int orchid = seedTaxon(200, QStringLiteral("Orchid"), QStringLiteral("species"), 10);
    seedMatch(seedCapture(QStringLiteral("a")), frog, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("b")), orchid, QStringLiteral("confirmed"));
    const int frogs = seedProject(QStringLiteral("Frogs"));
    const int orchids = seedProject(QStringLiteral("Orchids"));
    linkTaxon(frogs, frog);
    linkTaxon(orchids, orchid);

    UntreedMatchStore store(m_db->connectionName());
    QVERIFY(store.untreedTaxa().isEmpty());

    const QList<UntreedTaxon> notInFrogs = store.untreedTaxa(frogs);
    QCOMPARE(notInFrogs.size(), 1);
    QCOMPARE(notInFrogs[0].inatId, qint64(200));
}

void TestUntreedMatchStore::pendingRejectedAndUnmatchedAreExcluded()
{
    const int taxon = seedTaxon(100, QStringLiteral("Taxon"), QStringLiteral("species"), 10);
    seedMatch(seedCapture(QStringLiteral("a")), taxon, QStringLiteral("pending"));
    seedMatch(seedCapture(QStringLiteral("b")), taxon, QStringLiteral("rejected"));
    seedMatch(seedCapture(QStringLiteral("c")), 0, QStringLiteral("pending"));
    seedCapture(QStringLiteral("d"));   // no capture_match row at all

    QVERIFY(UntreedMatchStore(m_db->connectionName()).untreedTaxa().isEmpty());
}

void TestUntreedMatchStore::onlyTheTopMatchPerCaptureCounts()
{
    const int engineGuess = seedTaxon(100, QStringLiteral("Engine guess"), QStringLiteral("species"), 10);
    const int userPick = seedTaxon(200, QStringLiteral("User pick"), QStringLiteral("species"), 10);
    const int capture = seedCapture(QStringLiteral("a"));
    // The engine's match is more confident, but a user decision outranks it.
    seedMatch(capture, engineGuess, QStringLiteral("auto"), 0.99);
    seedMatch(capture, userPick, QStringLiteral("confirmed"), 0.5, QStringLiteral("user"));

    const QList<UntreedTaxon> list = UntreedMatchStore(m_db->connectionName()).untreedTaxa();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].inatId, qint64(200));
    QCOMPARE(list[0].photoCount, 1);
}

void TestUntreedMatchStore::genusMatchCarriesItsRank()
{
    const int genus = seedTaxon(300, QStringLiteral("Caladenia"), QStringLiteral("genus"), 20);
    const int species = seedTaxon(301, QStringLiteral("Caladenia flava"), QStringLiteral("species"), 10);
    seedMatch(seedCapture(QStringLiteral("a")), genus, QStringLiteral("auto"));
    seedMatch(seedCapture(QStringLiteral("b")), species, QStringLiteral("auto"));
    linkTaxon(seedProject(QStringLiteral("Orchids")), species);

    const QList<UntreedTaxon> list = UntreedMatchStore(m_db->connectionName()).untreedTaxa();
    QCOMPARE(list.size(), 1);
    const UntreedTaxon *t = find(list, 300);
    QVERIFY(t);
    QCOMPARE(t->rank, QStringLiteral("genus"));
    QCOMPARE(t->rankLevel, 20);
}

void TestUntreedMatchStore::aboveSpeciesListsGenusMatchesEvenInATree()
{
    const int genus = seedTaxon(300, QStringLiteral("Caladenia"), QStringLiteral("genus"), 20);
    const int family = seedTaxon(400, QStringLiteral("Orchidaceae"), QStringLiteral("family"), 30);
    const int species = seedTaxon(301, QStringLiteral("Caladenia flava"), QStringLiteral("species"), 10);
    const int subspecies =
        seedTaxon(302, QStringLiteral("Caladenia flava flava"), QStringLiteral("subspecies"), 5);
    seedMatch(seedCapture(QStringLiteral("a")), genus, QStringLiteral("auto"));
    seedMatch(seedCapture(QStringLiteral("b")), genus, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("c")), family, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("d")), species, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("e")), subspecies, QStringLiteral("confirmed"));
    seedMatch(seedCapture(QStringLiteral("f")), genus, QStringLiteral("pending"));
    const int orchids = seedProject(QStringLiteral("Orchids"));
    linkTaxon(orchids, genus);   // tree membership doesn't matter here
    linkTaxon(orchids, family);

    UntreedMatchStore store(m_db->connectionName());
    const QList<UntreedTaxon> list = store.aboveSpeciesTaxa();
    QVERIFY2(store.error().isEmpty(), qPrintable(store.error()));
    QCOMPARE(list.size(), 2);
    const UntreedTaxon *g = find(list, 300);
    QVERIFY(g);
    QCOMPARE(g->photoCount, 2);   // the pending match isn't counted
    QCOMPARE(g->confirmedCount, 1);
    QVERIFY(find(list, 400));
}

QTEST_MAIN(TestUntreedMatchStore)
#include "tst_untreedmatchstore.moc"
