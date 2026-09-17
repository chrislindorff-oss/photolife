#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "db/Database.h"

#include "PgTestDsn.h"

using namespace pl;

namespace {

// Duplicated from Database.cpp's discoverMigrations() rather than exposing
// it: this is a test-only sanity check that the two dialect subtrees never
// drift apart, not a code path the app itself relies on.
QSet<int> migrationVersions(const QString &resourceDir)
{
    QSet<int> versions;
    const QRegularExpression pattern(QStringLiteral("^(\\d+)_.*\\.sql$"));
    QDir dir(resourceDir);
    for (const QString &name : dir.entryList(QStringList{QStringLiteral("*.sql")}, QDir::Files)) {
        const QRegularExpressionMatch m = pattern.match(name);
        if (m.hasMatch())
            versions.insert(m.captured(1).toInt());
    }
    return versions;
}

} // namespace

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
    void sqliteAndPostgresMigrationsCoverTheSameVersions();
    void pgOpensAndMigrates();
    void pgReopeningIsIdempotent();
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

void TestDatabase::sqliteAndPostgresMigrationsCoverTheSameVersions()
{
    const QSet<int> sqliteVersions = migrationVersions(QStringLiteral(":/migrations/sqlite"));
    const QSet<int> postgresVersions = migrationVersions(QStringLiteral(":/migrations/postgres"));

    QVERIFY(!sqliteVersions.isEmpty());
    QCOMPARE(postgresVersions, sqliteVersions);
    QCOMPARE(Database::targetSchemaVersion(CatalogueDescriptor::Backend::Sqlite),
             Database::targetSchemaVersion(CatalogueDescriptor::Backend::Postgres));
}

void TestDatabase::pgOpensAndMigrates()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed database tests");

    Database db;
    QVERIFY2(db.open(*descriptor), qPrintable(db.error()));
    QVERIFY(db.isOpen());
    QCOMPARE(db.schemaVersion(),
             Database::targetSchemaVersion(CatalogueDescriptor::Backend::Postgres));
}

void TestDatabase::pgReopeningIsIdempotent()
{
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run Postgres-backed database tests");

    {
        Database first;
        QVERIFY2(first.open(*descriptor), qPrintable(first.error()));
        QCOMPARE(first.schemaVersion(),
                 Database::targetSchemaVersion(CatalogueDescriptor::Backend::Postgres));
    }

    Database second;
    QVERIFY2(second.open(*descriptor), qPrintable(second.error()));
    QCOMPARE(second.schemaVersion(),
             Database::targetSchemaVersion(CatalogueDescriptor::Backend::Postgres));
}

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
