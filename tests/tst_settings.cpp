#include <QtTest>

#include <QCoreApplication>
#include <QStandardPaths>

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
