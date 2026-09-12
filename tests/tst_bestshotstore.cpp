#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "catalogue/BestShotStore.h"
#include "db/Database.h"

using namespace pl;
using namespace pl::catalogue;

class TestBestShotStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void nominateAndRemoveIsIdempotent();
    void setBestShotsReportsRowsChanged();
    void deletingACaptureCascadesTheBestShotRow();

private:
    std::unique_ptr<Database> m_db;

    int seedCapture(const QString &baseName);
};

void TestBestShotStore::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral(
        "INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib', 'lib', 0)")));
}

void TestBestShotStore::cleanup()
{
    m_db.reset();
}

int TestBestShotStore::seedCapture(const QString &baseName)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "INSERT INTO capture (folder_id, base_name) VALUES (1, ?)"));
    q.addBindValue(baseName);
    q.exec();
    return q.lastInsertId().toInt();
}

void TestBestShotStore::nominateAndRemoveIsIdempotent()
{
    const int a = seedCapture(QStringLiteral("a"));
    const int b = seedCapture(QStringLiteral("b"));

    BestShotStore store(m_db->connectionName());

    QCOMPARE(store.setBestShots({a, b}, true), 2);
    QVERIFY(store.isBestShot(a));
    QVERIFY(store.isBestShot(b));

    // Re-nominating is a no-op.
    QCOMPARE(store.setBestShots({a, b}, true), 0);

    QCOMPARE(store.setBestShots({a}, false), 1);
    QVERIFY(!store.isBestShot(a));
    QVERIFY(store.isBestShot(b));

    // Removing one that isn't starred is a no-op.
    QCOMPARE(store.setBestShots({a}, false), 0);
}

void TestBestShotStore::setBestShotsReportsRowsChanged()
{
    const int a = seedCapture(QStringLiteral("a"));
    const int b = seedCapture(QStringLiteral("b"));

    BestShotStore store(m_db->connectionName());
    QCOMPARE(store.setBestShots({a}, true), 1);

    // a already starred, b not — only b is added.
    QCOMPARE(store.setBestShots({a, b}, true), 1);

    QCOMPARE(store.setBestShots({}, true), 0);
}

void TestBestShotStore::deletingACaptureCascadesTheBestShotRow()
{
    const int a = seedCapture(QStringLiteral("a"));

    BestShotStore store(m_db->connectionName());
    QCOMPARE(store.setBestShots({a}, true), 1);

    QSqlQuery del(QSqlDatabase::database(m_db->connectionName(), false));
    del.prepare(QStringLiteral("DELETE FROM capture WHERE id = ?"));
    del.addBindValue(a);
    QVERIFY(del.exec());

    QVERIFY(!store.isBestShot(a));

    QSqlQuery count(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM best_shot")));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 0);
}

QTEST_MAIN(TestBestShotStore)
#include "tst_bestshotstore.moc"
