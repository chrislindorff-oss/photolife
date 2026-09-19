#include <QtTest>

#include <QSignalSpy>

#include "db/Database.h"
#include "db/PostgresConnectionMonitor.h"

using namespace pl;

class TestPostgresConnectionMonitor : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void firstCheckAlwaysEmits();
    void repeatedChecksWithNoChangeDontReemit();
    void emitsAgainWhenConnectionCloses();

private:
    std::unique_ptr<Database> m_db;
};

void TestPostgresConnectionMonitor::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
}

void TestPostgresConnectionMonitor::cleanup()
{
    m_db.reset();
}

void TestPostgresConnectionMonitor::firstCheckAlwaysEmits()
{
    PostgresConnectionMonitor monitor(m_db->connectionName());
    QSignalSpy spy(&monitor, &PostgresConnectionMonitor::connectedChanged);

    monitor.checkNow();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
}

void TestPostgresConnectionMonitor::repeatedChecksWithNoChangeDontReemit()
{
    PostgresConnectionMonitor monitor(m_db->connectionName());
    QSignalSpy spy(&monitor, &PostgresConnectionMonitor::connectedChanged);

    monitor.checkNow();
    monitor.checkNow();
    monitor.checkNow();

    QCOMPARE(spy.count(), 1);   // only the first call's state change is reported
}

void TestPostgresConnectionMonitor::emitsAgainWhenConnectionCloses()
{
    PostgresConnectionMonitor monitor(m_db->connectionName());
    QSignalSpy spy(&monitor, &PostgresConnectionMonitor::connectedChanged);

    monitor.checkNow();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    m_db->close();
    monitor.checkNow();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

QTEST_GUILESS_MAIN(TestPostgresConnectionMonitor)
#include "tst_postgresconnectionmonitor.moc"
