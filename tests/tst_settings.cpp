#include <QtTest>

#include <QCoreApplication>
#include <QStandardPaths>

#include "db/CatalogueDescriptor.h"
#include "pl/Version.h"
#include "settings/Settings.h"

using namespace pl;

class TestSettings : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void watchedRootsRoundTrip();
    void databasePathDefaultsToAppData();
    void databasePathHonoursOverride();
    void catalogueDescriptorDefaultsToSqlite();
    void catalogueDescriptorRoundTripsPostgres();
    void catalogueDescriptorSwitchingBackToSqliteKeepsDatabasePath();
    void windowStateRoundTrip();
    void captureCaptionFieldsDefaultsToNameOnly();
    void captureCaptionFieldsRoundTrip();
    void inatSettingsDefaultEmptyAndRoundTrip();

private:
    // Wipes any values a previous test wrote to the (test-mode) store.
    void clearStore() { QSettings().clear(); }
};

void TestSettings::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QString::fromLatin1(kOrgName));
    QCoreApplication::setOrganizationDomain(QString::fromLatin1(kOrgDomain));
    QCoreApplication::setApplicationName(QString::fromLatin1(kAppName));
}

void TestSettings::init()
{
    clearStore();
}

void TestSettings::watchedRootsRoundTrip()
{
    const QStringList roots{QStringLiteral("/photos/2024"), QStringLiteral("/photos/2025")};

    Settings writer;
    writer.setWatchedRoots(roots);

    QCOMPARE(Settings().watchedRoots(), roots);
}

void TestSettings::databasePathDefaultsToAppData()
{
    const QString path = Settings().databasePath();
    QVERIFY(path.endsWith(QStringLiteral("catalogue.db")));
    QVERIFY(path.contains(QString::fromLatin1(kAppName)));
}

void TestSettings::databasePathHonoursOverride()
{
    Settings writer;
    writer.setDatabasePath(QStringLiteral("/srv/library/custom.db"));
    QCOMPARE(Settings().databasePath(), QStringLiteral("/srv/library/custom.db"));

    writer.setDatabasePath(QString());
    QVERIFY(Settings().databasePath().endsWith(QStringLiteral("catalogue.db")));
}

void TestSettings::catalogueDescriptorDefaultsToSqlite()
{
    const CatalogueDescriptor d = Settings().catalogueDescriptor();
    QCOMPARE(d.backend, CatalogueDescriptor::Backend::Sqlite);
    QVERIFY(d.sqlitePath.endsWith(QStringLiteral("catalogue.db")));
}

void TestSettings::catalogueDescriptorRoundTripsPostgres()
{
    CatalogueDescriptor d;
    d.backend = CatalogueDescriptor::Backend::Postgres;
    d.pgHost = QStringLiteral("db.example.com");
    d.pgPort = 6543;
    d.pgDbName = QStringLiteral("photolife");
    d.pgUser = QStringLiteral("helena");
    d.pgPassword = QStringLiteral("s3cret");
    d.pgSslMode = QStringLiteral("require");

    Settings writer;
    writer.setCatalogueDescriptor(d);

    const CatalogueDescriptor read = Settings().catalogueDescriptor();
    QCOMPARE(read.backend, CatalogueDescriptor::Backend::Postgres);
    QCOMPARE(read.pgHost, d.pgHost);
    QCOMPARE(read.pgPort, d.pgPort);
    QCOMPARE(read.pgDbName, d.pgDbName);
    QCOMPARE(read.pgUser, d.pgUser);
    QCOMPARE(read.pgPassword, d.pgPassword);
    QCOMPARE(read.pgSslMode, d.pgSslMode);
}

void TestSettings::catalogueDescriptorSwitchingBackToSqliteKeepsDatabasePath()
{
    Settings writer;
    writer.setDatabasePath(QStringLiteral("/srv/library/custom.db"));

    CatalogueDescriptor postgres;
    postgres.backend = CatalogueDescriptor::Backend::Postgres;
    postgres.pgHost = QStringLiteral("db.example.com");
    writer.setCatalogueDescriptor(postgres);
    QCOMPARE(Settings().catalogueDescriptor().backend, CatalogueDescriptor::Backend::Postgres);

    writer.setCatalogueDescriptor(CatalogueDescriptor::sqlite(QStringLiteral("/srv/library/custom.db")));
    const CatalogueDescriptor back = Settings().catalogueDescriptor();
    QCOMPARE(back.backend, CatalogueDescriptor::Backend::Sqlite);
    QCOMPARE(back.sqlitePath, QStringLiteral("/srv/library/custom.db"));
}

void TestSettings::windowStateRoundTrip()
{
    const QByteArray geometry = QByteArrayLiteral("\x01\x02geometry");
    const QByteArray state = QByteArrayLiteral("\x03\x04state");

    Settings writer;
    writer.setMainWindowGeometry(geometry);
    writer.setMainWindowState(state);

    Settings reader;
    QCOMPARE(reader.mainWindowGeometry(), geometry);
    QCOMPARE(reader.mainWindowState(), state);
}

void TestSettings::captureCaptionFieldsDefaultsToNameOnly()
{
    QCOMPARE(Settings().captureCaptionFields(), 1);   // CaptionName
}

void TestSettings::captureCaptionFieldsRoundTrip()
{
    Settings writer;
    writer.setCaptureCaptionFields(0b10110);   // taxon + date + filetype, say

    QCOMPARE(Settings().captureCaptionFields(), 0b10110);
}

void TestSettings::inatSettingsDefaultEmptyAndRoundTrip()
{
    QVERIFY(Settings().inatUsername().isEmpty());
    QVERIFY(Settings().inatApiToken().isEmpty());

    Settings writer;
    writer.setInatUsername(QStringLiteral("some_observer"));
    writer.setInatApiToken(QStringLiteral("test-token-value"));

    Settings reader;
    QCOMPARE(reader.inatUsername(), QStringLiteral("some_observer"));
    QCOMPARE(reader.inatApiToken(), QStringLiteral("test-token-value"));
}

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
