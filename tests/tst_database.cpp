#include <QtTest>

#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "db/Database.h"

using namespace pl;

class TestDatabase : public QObject
{
    Q_OBJECT

private slots:
    void targetSchemaIsAtLeastOne();
    void opensInMemoryAndMigrates();
    void createsParentDirsAndMigratesOnDisk();
    void reopeningExistingCatalogueIsIdempotent();
    void reportsErrorWhenPathCannotBeCreated();
    void closeLeavesDatabaseNotOpen();
};

void TestDatabase::targetSchemaIsAtLeastOne()
{
    QVERIFY(Database::targetSchemaVersion() >= 1);
}

void TestDatabase::opensInMemoryAndMigrates()
{
    Database db;
    QVERIFY2(db.open(QStringLiteral(":memory:")), qPrintable(db.error()));
    QVERIFY(db.isOpen());
    QCOMPARE(db.schemaVersion(), Database::targetSchemaVersion());
}

void TestDatabase::createsParentDirsAndMigratesOnDisk()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("nested/dir/catalogue.db"));

    Database db;
    QVERIFY2(db.open(path), qPrintable(db.error()));
    QCOMPARE(db.schemaVersion(), Database::targetSchemaVersion());
    QVERIFY(QFileInfo::exists(path));
}

void TestDatabase::reopeningExistingCatalogueIsIdempotent()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("catalogue.db"));

    {
        Database first;
        QVERIFY2(first.open(path), qPrintable(first.error()));
        QCOMPARE(first.schemaVersion(), Database::targetSchemaVersion());
    }

    Database second;
    QVERIFY2(second.open(path), qPrintable(second.error()));
    QCOMPARE(second.schemaVersion(), Database::targetSchemaVersion());
}

void TestDatabase::reportsErrorWhenPathCannotBeCreated()
{
    // A regular file stands where a directory would need to be.
    QTemporaryFile blocker;
    QVERIFY(blocker.open());
    const QString path = blocker.fileName() + QStringLiteral("/catalogue.db");

    Database db;
    QVERIFY(!db.open(path));
    QVERIFY(!db.error().isEmpty());
    QVERIFY(!db.isOpen());
}

void TestDatabase::closeLeavesDatabaseNotOpen()
{
    Database db;
    QVERIFY2(db.open(QStringLiteral(":memory:")), qPrintable(db.error()));
    db.close();
    QVERIFY(!db.isOpen());
    QCOMPARE(db.schemaVersion(), -1);
}

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
