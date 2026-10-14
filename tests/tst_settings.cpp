#include <QtTest>

#include <QCoreApplication>
#include <QDir>
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
    void databasePathExpandsLeadingTilde();
    void catalogueDescriptorDefaultsToSqlite();
    void startsWithOnlyTheLocalCatalogue();
    void serverProfileHasItsOwnFileFoldersAndSources();
    void migratesEachOldSettingsShape_data();
    void migratesEachOldSettingsShape();
    void removingTheActiveServerFallsBackToLocal();
    void photoStorageRoundTrip();
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

void TestSettings::databasePathExpandsLeadingTilde()
{
    // Qt never expands a shell-style "~" itself -- a path stored (typed by
    // the user, or hand-edited into the config file) with a literal leading
    // "~" must resolve to the real home directory, not a directory literally
    // named "~" wherever the app happens to be launched from.
    Settings writer;
    writer.setDatabasePath(QStringLiteral("~/library/custom.db"));
    QCOMPARE(Settings().databasePath(),
             QDir::homePath() + QStringLiteral("/library/custom.db"));

    writer.setDatabasePath(QStringLiteral("~"));
    QCOMPARE(Settings().databasePath(), QDir::homePath());

    // A path that merely contains a "~" elsewhere (not a leading shortcut)
    // is left alone.
    writer.setDatabasePath(QStringLiteral("/srv/lib~rary/custom.db"));
    QCOMPARE(Settings().databasePath(), QStringLiteral("/srv/lib~rary/custom.db"));
}

void TestSettings::catalogueDescriptorDefaultsToSqlite()
{
    const CatalogueDescriptor d = Settings().catalogueDescriptor();
    QCOMPARE(d.backend, CatalogueDescriptor::Backend::Sqlite);
    QVERIFY(d.sqlitePath.endsWith(QStringLiteral("catalogue.db")));
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

void TestSettings::startsWithOnlyTheLocalCatalogue()
{
    Settings s;
    QCOMPARE(s.profiles().size(), 1);
    QVERIFY(s.activeProfile().isLocal());
    QVERIFY(!s.syncEnabled());
    QVERIFY(!s.syncEditing());
    QVERIFY(s.databasePath().endsWith(QStringLiteral("/catalogue.db")));
    QVERIFY(!s.databasePath().contains(QStringLiteral("/servers/")));
}

void TestSettings::serverProfileHasItsOwnFileFoldersAndSources()
{
    Settings s;
    const QString localDb = s.databasePath();
    s.setWatchedRoots({QStringLiteral("/home/me/Photos")});
    s.setSourceSyncedAt(QStringLiteral("ala"), QDateTime(QDate(2026, 1, 1), QTime(0, 0)));

    Settings::CatalogueProfile club;
    club.server = true;
    club.name = QStringLiteral("Nature Photo Club");
    club.pg.pgHost = QStringLiteral("db.example.com");
    club.pg.pgDbName = QStringLiteral("club");
    club.pg.pgUser = QStringLiteral("alice");
    club.userName = QStringLiteral("Alice");
    club.libraryRoot = QStringLiteral("/mnt/gdrive/Club Photos");
    const QString id = s.saveServerProfile(club);
    QCOMPARE(id, QStringLiteral("nature-photo-club"));
    // Saving without an id adds another (with its own id); with one, updates it.
    QCOMPARE(s.saveServerProfile(club), QStringLiteral("nature-photo-club-2"));
    QCOMPARE(s.profiles().size(), 3);
    s.removeServerProfile(QStringLiteral("nature-photo-club-2"));
    club.id = id;
    club.libraryRoot = QStringLiteral("/mnt/gdrive/Club Photos");
    QCOMPARE(s.saveServerProfile(club), id);
    QCOMPARE(s.profiles().size(), 2);

    s.setActiveProfile(id);
    QVERIFY(s.syncEnabled());
    QCOMPARE(s.activeProfile().name, club.name);
    QVERIFY(s.databasePath().endsWith(QStringLiteral("/servers/nature-photo-club/catalogue.db")));
    QCOMPARE(s.catalogueDescriptor().sqlitePath, s.databasePath());   // opened locally
    QCOMPARE(s.syncServerDescriptor().pgHost, club.pg.pgHost);
    QCOMPARE(s.libraryRoot(), club.libraryRoot);
    QCOMPARE(s.syncUserName(), QStringLiteral("Alice"));
    // Its own folders, download times and editing flag.
    QVERIFY(s.watchedRoots().isEmpty());
    QVERIFY(!s.sourceSyncedAt(QStringLiteral("ala")).isValid());
    s.setWatchedRoots({QStringLiteral("/mnt/gdrive/Club Photos/Birds")});
    s.setSyncEditing(true);

    // Back to Local: untouched.
    s.setActiveProfile(QStringLiteral("local"));
    QCOMPARE(s.databasePath(), localDb);
    QCOMPARE(s.watchedRoots(), QStringList{QStringLiteral("/home/me/Photos")});
    QVERIFY(s.sourceSyncedAt(QStringLiteral("ala")).isValid());
    QVERIFY(!s.syncEditing());
    // And the server kept its own.
    s.setActiveProfile(id);
    QCOMPARE(s.watchedRoots(), QStringList{QStringLiteral("/mnt/gdrive/Club Photos/Birds")});
    QVERIFY(s.syncEditing());
    QCOMPARE(Settings().activeProfile().id, id);   // persisted
}

void TestSettings::migratesEachOldSettingsShape_data()
{
    QTest::addColumn<QString>("backend");
    QTest::addColumn<bool>("withServer");
    QTest::addColumn<bool>("serverActive");
    QTest::newRow("local only") << QStringLiteral("sqlite") << false << false;
    QTest::newRow("local, a server set up earlier") << QStringLiteral("sqlite") << true << false;
    QTest::newRow("old direct postgres") << QStringLiteral("postgres") << true << true;
    QTest::newRow("sync mode") << QStringLiteral("sync") << true << true;
}

void TestSettings::migratesEachOldSettingsShape()
{
    QFETCH(QString, backend);
    QFETCH(bool, withServer);
    QFETCH(bool, serverActive);
    {
        QSettings raw;
        raw.setValue(QStringLiteral("library/backend"), backend);
        raw.setValue(QStringLiteral("library/watchedRoots"), QStringList{QStringLiteral("/home/me/Photos")});
        if (withServer) {
            raw.setValue(QStringLiteral("library/pg/host"), QStringLiteral("ep-x.neon.tech"));
            raw.setValue(QStringLiteral("library/pg/dbName"), QStringLiteral("photolifedb"));
            raw.setValue(QStringLiteral("library/pg/user"), QStringLiteral("owner"));
            raw.setValue(QStringLiteral("library/sync/userName"), QStringLiteral("chrisl"));
            raw.setValue(QStringLiteral("library/sync/libraryRoot"), QStringLiteral("/home/me/Photos"));
            raw.setValue(QStringLiteral("library/sync/editing"), true);
        }
    }
    Settings s;
    const QString localDb = s.profileDatabasePath(QStringLiteral("local"));
    QVERIFY(s.migrateProfiles());
    QVERIFY(!s.migrateProfiles());   // once only

    QCOMPARE(s.profiles().size(), withServer ? 2 : 1);
    QCOMPARE(s.syncEnabled(), serverActive);
    QVERIFY(!s.syncEditing());   // a fresh working copy: nothing is being edited yet
    if (withServer) {
        const auto server = s.profile(QStringLiteral("photolifedb"));
        QVERIFY(server);
        QCOMPARE(server->pg.pgHost, QStringLiteral("ep-x.neon.tech"));
        QCOMPARE(server->userName, QStringLiteral("chrisl"));
        QCOMPARE(server->libraryRoot, QStringLiteral("/home/me/Photos"));
    }
    // The existing catalogue file and folders stay the Local catalogue's.
    s.setActiveProfile(QStringLiteral("local"));
    QCOMPARE(s.databasePath(), localDb);
    QCOMPARE(s.watchedRoots(), QStringList{QStringLiteral("/home/me/Photos")});
}

void TestSettings::removingTheActiveServerFallsBackToLocal()
{
    Settings s;
    Settings::CatalogueProfile p;
    p.server = true;
    p.name = QStringLiteral("Club");
    p.pg.pgHost = QStringLiteral("h");
    const QString id = s.saveServerProfile(p);
    s.setActiveProfile(id);
    QVERIFY(s.syncEnabled());
    s.removeServerProfile(id);
    QVERIFY(!s.syncEnabled());
    QCOMPARE(s.profiles().size(), 1);
    s.setActiveProfile(QStringLiteral("no-such-server"));
    QVERIFY(s.activeProfile().isLocal());
}

void TestSettings::photoStorageRoundTrip()
{
    Settings s;
    Settings::CatalogueProfile p;
    p.server = true;
    p.name = QStringLiteral("Club");
    p.libraryRoot = QStringLiteral("/photos");
    const QString plain = s.saveServerProfile(p);
    QCOMPARE(s.profile(plain)->photoStore, QString::fromLatin1(Settings::kPhotoStoreNone));
    QVERIFY(!s.profile(plain)->syncsPhotos());

    p.id.clear();
    p.name = QStringLiteral("Club with R2");
    p.photoStore = QString::fromLatin1(Settings::kPhotoStoreS3);
    p.s3.endpoint = QStringLiteral("https://acct.r2.cloudflarestorage.com");
    p.s3.region = QStringLiteral("auto");
    p.s3.bucket = QStringLiteral("club-photos");
    p.s3.prefix = QStringLiteral("orchids");
    p.s3.accessKey = QStringLiteral("AKID");
    p.s3.secretKey = QStringLiteral("s3cr3t");
    const auto back = s.profile(s.saveServerProfile(p));
    QVERIFY(back && back->syncsPhotos());
    QCOMPARE(back->s3.endpoint, p.s3.endpoint);
    QCOMPARE(back->s3.bucket, p.s3.bucket);
    QCOMPARE(back->s3.prefix, p.s3.prefix);
    QCOMPARE(back->s3.accessKey, p.s3.accessKey);
    QCOMPARE(back->s3.secretKey, p.s3.secretKey);
    QVERIFY(back->s3.isComplete());
    QVERIFY(back->photosAllTrees);

    Settings::CatalogueProfile some = *back;
    some.photosAllTrees = false;
    some.photoTrees = {3, 7};
    const auto again = s.profile(s.saveServerProfile(some));
    QVERIFY(!again->photosAllTrees);
    QCOMPARE(again->photoTrees, (QList<int>{3, 7}));
}

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
