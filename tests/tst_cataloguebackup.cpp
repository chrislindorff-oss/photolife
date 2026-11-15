#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/CatalogueBackup.h"
#include "db/Database.h"

using namespace pl;

class TestCatalogueBackup : public QObject
{
    Q_OBJECT

private slots:
    void backupIsACompleteOwnerOnlyCopy();
    void keepsTheNewestTenAndNothingElseIsTouched();
    void aRefusedBackupStopsTheUpgrade();
    void aNewCatalogueNeedsNoBackup();

private:
    // A catalogue one version behind: as the previous PhotoLife left it.
    QString olderCatalogue(QTemporaryDir &dir, int *version);
    static int versionOf(const QString &path);
};

int TestCatalogueBackup::versionOf(const QString &path)
{
    int v = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("peek"));
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")) && q.next())
                v = q.value(0).toInt();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("peek"));
    return v;
}

QString TestCatalogueBackup::olderCatalogue(QTemporaryDir &dir, int *version)
{
    const QString path = dir.filePath(QStringLiteral("catalogue.db"));
    {
        Database db;
        if (!db.open(path))
            return {};
        QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
        q.exec(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES ('/photos', 'photos', 0)"));
        // Undo the latest migration (027: Unsorted folders, time_checked).
        q.exec(QStringLiteral("DROP TABLE unsorted_photo"));
        q.exec(QStringLiteral("DROP TABLE unsorted_folder"));
        q.exec(QStringLiteral("ALTER TABLE capture DROP COLUMN time_checked"));
        q.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version = 27"));
    }
    *version = versionOf(path);
    return path;
}

void TestCatalogueBackup::backupIsACompleteOwnerOnlyCopy()
{
    QTemporaryDir dir;
    Database db;
    QVERIFY(db.open(dir.filePath(QStringLiteral("catalogue.db"))));
    QSqlQuery(QSqlDatabase::database(db.connectionName(), false))
        .exec(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES ('/photos', 'photos', 0)"));
    const QString folder = dir.filePath(QStringLiteral("backups"));
    const CatalogueBackupResult r =
        backupCatalogue(db.connectionName(), folder, 23, 24, 10, QDateTime(QDate(2026, 11, 14), QTime(9, 30, 0)));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(QFileInfo(r.path).fileName(), QStringLiteral("PhotoLife catalogue v23 before v24 2026-11-14 093000.db"));
#ifndef Q_OS_WIN
    QCOMPARE(QFile::permissions(r.path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
#endif
    {
        QSqlDatabase copy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("copy"));
        copy.setDatabaseName(r.path);
        QVERIFY(copy.open());
        QSqlQuery q(copy);
        QVERIFY(q.exec(QStringLiteral("SELECT path FROM folder")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("/photos"));
        copy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("copy"));
    // The same second again: a name of its own.
    const CatalogueBackupResult again =
        backupCatalogue(db.connectionName(), folder, 23, 24, 10, QDateTime(QDate(2026, 11, 14), QTime(9, 30, 0)));
    QVERIFY(again.ok);
    QVERIFY(again.path.endsWith(QStringLiteral("093000 (2).db")));
    QCOMPARE(catalogueBackups(folder).first(), again.path);
}

void TestCatalogueBackup::keepsTheNewestTenAndNothingElseIsTouched()
{
    QTemporaryDir dir;
    Database db;
    QVERIFY(db.open(dir.filePath(QStringLiteral("catalogue.db"))));
    const QString folder = dir.filePath(QStringLiteral("backups"));
    QDir().mkpath(folder);
    // Not PhotoLife's backups: never removed.
    for (const char *other : {"notes.db", "PhotoLife catalogue notes.db", "my catalogue copy.db"}) {
        QFile f(QDir(folder).filePath(QString::fromLatin1(other)));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    for (int day = 1; day <= 12; ++day) {
        const CatalogueBackupResult r = backupCatalogue(db.connectionName(), folder, 20 + day, 21 + day, 10,
                                                        QDateTime(QDate(2026, 1, day), QTime(12, 0)));
        QVERIFY(r.ok);
        if (day == 11)
            QCOMPARE(r.removed.size(), 1);
    }
    const QStringList kept = catalogueBackups(folder);
    QCOMPARE(kept.size(), 10);
    QVERIFY(kept.first().contains(QStringLiteral("2026-01-12")));
    QVERIFY(kept.last().contains(QStringLiteral("2026-01-03")));
    for (const char *other : {"notes.db", "PhotoLife catalogue notes.db", "my catalogue copy.db"})
        QVERIFY(QFile::exists(QDir(folder).filePath(QString::fromLatin1(other))));
}

void TestCatalogueBackup::aRefusedBackupStopsTheUpgrade()
{
    QTemporaryDir dir;
    int older = 0;
    const QString path = olderCatalogue(dir, &older);
    QCOMPARE(older, Database::targetSchemaVersion() - 1);

    int calls = 0;
    {
        Database db;
        db.setBeforeUpgrade([&](const QString &, int from, int to) {
            ++calls;
            [&] { QCOMPARE(from, older); QCOMPARE(to, Database::targetSchemaVersion()); }();
            return false;
        });
        QVERIFY(!db.open(path));
        QVERIFY(db.error().contains(QStringLiteral("wasn't upgraded")));
    }
    QCOMPARE(calls, 1);
    QCOMPARE(versionOf(path), older);   // left as it was

    // Backed up, then upgraded; the backup is the older catalogue.
    const QString folder = dir.filePath(QStringLiteral("backups"));
    QString made;
    {
        Database db;
        db.setBeforeUpgrade([&](const QString &connection, int from, int to) {
            const CatalogueBackupResult r = backupCatalogue(connection, folder, from, to);
            made = r.path;
            return r.ok;
        });
        QVERIFY2(db.open(path), qPrintable(db.error()));
        QCOMPARE(db.schemaVersion(), Database::targetSchemaVersion());
    }
    QVERIFY(QFile::exists(made));
    QCOMPARE(versionOf(made), older);
}

void TestCatalogueBackup::aNewCatalogueNeedsNoBackup()
{
    QTemporaryDir dir;
    int calls = 0;
    {
        Database db;
        db.setBeforeUpgrade([&](const QString &, int, int) { ++calls; return false; });
        QVERIFY(db.open(dir.filePath(QStringLiteral("new.db"))));
        // Opened again, up to date: nothing to upgrade either.
        db.close();
        QVERIFY(db.open(dir.filePath(QStringLiteral("new.db"))));
    }
    QCOMPARE(calls, 0);
}

QTEST_GUILESS_MAIN(TestCatalogueBackup)
#include "tst_cataloguebackup.moc"
