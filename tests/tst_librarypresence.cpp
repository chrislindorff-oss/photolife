#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "db/Database.h"
#include "inat/LibraryPresence.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::inat;

class TestLibraryPresence : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void treeMembershipIsExact();
    void matchedTaxonAndItsAncestorsAreInLibrary();
    void unmatchedFilenameCountsByScientificOrCommonName();
    void pendingAndRejectedMatchesDontCount();
    void annotateFillsCandidates();

private:
    std::unique_ptr<Database> m_db;

    QSqlQuery query() const { return QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false)); }
    void addTaxon(qint64 id, qint64 parent, const QString &rank, const QString &name);
    qint64 localId(qint64 inatId);
    int addCapture(const QString &nameText);
    void match(int captureId, qint64 inatId, const QString &status);
};

void TestLibraryPresence::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    QVERIFY(query().exec(QStringLiteral(
        "INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)")));

    addTaxon(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    addTaxon(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    addTaxon(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    addTaxon(102, 101, QStringLiteral("variety"), QStringLiteral("Caladenia carnea gigantea"));
    addTaxon(200, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));
    addTaxon(201, 200, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    taxonomy::TaxonomyStore store(m_db->connectionName());
    const int pid = store.ensureProject(QStringLiteral("Orchids"), 1, std::nullopt,
                                        QStringLiteral("inat"));
    QVERIFY(store.addProjectTaxon(pid, 201, true, false));
}

void TestLibraryPresence::cleanup()
{
    m_db.reset();
}

void TestLibraryPresence::addTaxon(qint64 id, qint64 parent, const QString &rank,
                                   const QString &name)
{
    taxonomy::TaxonomyStore store(m_db->connectionName());
    taxonomy::Taxon t;
    t.inatId = id;
    if (parent > 0)
        t.parentInatId = parent;
    t.rank = rank;
    t.name = name;
    store.upsertTaxon(t);
}

qint64 TestLibraryPresence::localId(qint64 inatId)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral("SELECT id FROM taxon WHERE inat_id = ?"));
    q.addBindValue(inatId);
    q.exec();
    q.next();
    return q.value(0).toLongLong();
}

int TestLibraryPresence::addCapture(const QString &nameText)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name, name_text) VALUES (1, ?, ?)"));
    q.addBindValue(nameText);
    q.addBindValue(nameText);
    q.exec();
    return q.lastInsertId().toInt();
}

void TestLibraryPresence::match(int captureId, qint64 inatId, const QString &status)
{
    QSqlQuery q = query();
    q.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
        "VALUES (?, ?, 'file', 0.95, ?)"));
    q.addBindValue(captureId);
    q.addBindValue(localId(inatId));
    q.addBindValue(status);
    QVERIFY(q.exec());
}

void TestLibraryPresence::treeMembershipIsExact()
{
    LibraryPresence p(m_db->connectionName());
    p.load();
    QVERIFY(p.inAnyTree(201));
    QVERIFY(!p.inAnyTree(101));
    QVERIFY(!p.inAnyTree(999));   // not even in the taxonomy cache
}

void TestLibraryPresence::matchedTaxonAndItsAncestorsAreInLibrary()
{
    match(addCapture(QStringLiteral("Caladenia carnea gigantea")), 102, QStringLiteral("confirmed"));

    LibraryPresence p(m_db->connectionName());
    p.load();
    QVERIFY(p.inLibrary(102, {}, {}));   // the variety itself
    QVERIFY(p.inLibrary(101, {}, {}));   // its species
    QVERIFY(p.inLibrary(100, {}, {}));   // and genus
    QVERIFY(!p.inLibrary(201, {}, {}));  // an unrelated species
}

void TestLibraryPresence::unmatchedFilenameCountsByScientificOrCommonName()
{
    addCapture(QStringLiteral("Diuris pardina"));
    addCapture(QStringLiteral("Pacific Black Duck"));

    LibraryPresence p(m_db->connectionName());
    p.load();
    QVERIFY(p.inLibrary(201, QStringLiteral("Diuris pardina"), {}));
    QVERIFY(p.inLibrary(555, QStringLiteral("Anas superciliosa"),
                        QStringLiteral("Pacific Black Duck")));
    QVERIFY(!p.inLibrary(556, QStringLiteral("Anas gracilis"), QStringLiteral("Grey Teal")));
}

void TestLibraryPresence::pendingAndRejectedMatchesDontCount()
{
    match(addCapture(QStringLiteral("mystery orchid")), 101, QStringLiteral("pending"));
    match(addCapture(QStringLiteral("another")), 201, QStringLiteral("rejected"));

    LibraryPresence p(m_db->connectionName());
    p.load();
    QVERIFY(!p.inLibrary(101, {}, {}));
    QVERIFY(!p.inLibrary(201, {}, {}));
}

void TestLibraryPresence::annotateFillsCandidates()
{
    match(addCapture(QStringLiteral("Caladenia carnea")), 101, QStringLiteral("auto"));

    Candidate have;
    have.observation.taxonInatId = 101;
    have.observation.taxonName = QStringLiteral("Caladenia carnea");
    Candidate fresh;
    fresh.observation.taxonInatId = 201;
    fresh.observation.taxonName = QStringLiteral("Diuris pardina");
    QList<Candidate> list = {have, fresh};

    LibraryPresence::annotate(m_db->connectionName(), list);
    QVERIFY(list[0].presenceChecked);
    QVERIFY(list[0].inLibrary);
    QVERIFY(!list[0].inAnyTree);
    QVERIFY(!list[1].inLibrary);
    QVERIFY(list[1].inAnyTree);
}

QTEST_GUILESS_MAIN(TestLibraryPresence)
#include "tst_librarypresence.moc"
