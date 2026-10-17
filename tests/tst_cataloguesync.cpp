#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "PgTestDsn.h"
#include "db/Database.h"
#include "sync/CatalogueSync.h"
#include "sync/SyncService.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::sync;

namespace {

const SyncIdentity kAlice{QStringLiteral("Alice"), QStringLiteral("alice-pc")};
const SyncIdentity kBob{QStringLiteral("Bob"), QStringLiteral("bob-laptop")};
const QString kAliceRoot = QStringLiteral("/home/alice/Nature Photos");
const QString kBobRoot = QStringLiteral("D:/Pictures/Nature");

QSqlQuery exec(const QString &connection, const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(connection, false));
    if (!q.exec(sql))
        qWarning().noquote() << "SQL failed:" << sql << q.lastError().text();
    return q;
}

int scalar(const QString &connection, const QString &sql)
{
    QSqlQuery q = exec(connection, sql);
    return q.next() ? q.value(0).toInt() : -1;
}

// Every content table's rows, as sorted text -- with each machine's library
// root replaced by a marker, so two machines' copies compare equal.
QStringList dump(const QString &connection, const QString &root)
{
    QStringList out;
    QSqlQuery tables = exec(connection, QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "AND name NOT IN ('schema_migrations', 'catalogue_sync') ORDER BY name"));
    while (tables.next()) {
        const QString t = tables.value(0).toString();
        QSqlQuery rows = exec(connection, QStringLiteral("SELECT * FROM \"%1\"").arg(t));
        QStringList lines;
        while (rows.next()) {
            QStringList values;
            for (int c = 0; c < rows.record().count(); ++c)
                values << (rows.value(c).isNull() ? QStringLiteral("∅")
                                                  : rows.value(c).toString());
            lines << t + QLatin1Char('|') + values.join(QLatin1Char('|')).replace(root, QStringLiteral("<ROOT>"));
        }
        lines.sort();
        out += lines;
    }
    return out;
}

} // namespace

class TestCatalogueSync : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void publishThenPullGivesTheSameCatalogue_data() { backends(); }
    void publishThenPullGivesTheSameCatalogue();
    void pushSendsOnlyWhatChanged_data() { backends(); }
    void pushSendsOnlyWhatChanged();
    void pushKeepsForeignKeyOrder_data() { backends(); }
    void pushKeepsForeignKeyOrder();
    void lockDecidesWhoMayPush_data() { backends(); }
    void lockDecidesWhoMayPush();
    void pathsOutsideTheRootAreKeptAndCounted_data() { backends(); }
    void pathsOutsideTheRootAreKeptAndCounted();
    void changingTheRootResendsMovedPaths_data() { backends(); }
    void changingTheRootResendsMovedPaths();
    void pullRefusesAnotherSchemaVersion_data() { backends(); }
    void pullRefusesAnotherSchemaVersion();
    void pathMappingRoundTrips();
    void applyingAPullKeepsBackups();
    void discardGoesBackToTheLastSync();
    void restoreKeepsTheSyncPosition();

private:
    void backends();
    bool openServer();
    std::unique_ptr<Database> openLocal(const QString &name);
    void populate(const QString &connection, const QString &root);
    CatalogueSync syncFor(const Database &local, const QString &root)
    {
        return CatalogueSync(local.connectionName(), m_server->connectionName(), PathMapping(root));
    }
    QString base(const QString &name) const { return m_dir->filePath(name + QStringLiteral(".base.db")); }

    // catalogue_sync.version as recorded in a sync snapshot file.
    static qint64 snapshotVersion(const QString &path)
    {
        qint64 version = -1;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("snapshot-check"));
            db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec(QStringLiteral("SELECT version FROM catalogue_sync WHERE id = 1"))
                    && q.next())
                    version = q.value(0).toLongLong();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("snapshot-check"));
        return version;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_server;
};

void TestCatalogueSync::backends()
{
    QTest::addColumn<bool>("postgres");
    QTest::newRow("sqlite server") << false;
    QTest::newRow("postgres server") << true;
}

void TestCatalogueSync::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
}

void TestCatalogueSync::cleanup()
{
    m_server.reset();
    m_dir.reset();
}

// The shared catalogue: a SQLite file, or the Postgres test database (emptied).
bool TestCatalogueSync::openServer()
{
    QFETCH(bool, postgres);
    m_server = std::make_unique<Database>();
    if (!postgres)
        return m_server->open(m_dir->filePath(QStringLiteral("server.db")));
    const auto descriptor = test::pgTestDescriptorFromEnv();
    if (!descriptor)
        return false;
    if (!m_server->open(*descriptor))
        return false;
    const QString c = m_server->connectionName();
    exec(c, QStringLiteral(
        "TRUNCATE best_shot, capture_keyword_hint, capture_match, capture, folder, rendition, "
        "geocode_cache, name_alias, place, project, project_build_checkpoint, "
        "project_excluded_taxon, project_taxon, representative, taxon, taxon_name, "
        "taxon_origin, taxon_status RESTART IDENTITY CASCADE"));
    exec(c, QStringLiteral("UPDATE catalogue_sync SET version = 0, locked_by = NULL, "
                           "locked_machine = NULL, locked_at = NULL WHERE id = 1"));
    return true;
}

std::unique_ptr<Database> TestCatalogueSync::openLocal(const QString &name)
{
    auto db = std::make_unique<Database>();
    if (!db->open(m_dir->filePath(name + QStringLiteral(".db"))))
        return nullptr;
    return db;
}

// A small but realistic catalogue: a tree with names and statuses, photo
// folders, captures, renditions and matches.
void TestCatalogueSync::populate(const QString &connection, const QString &root)
{
    taxonomy::TaxonomyStore store(connection);
    auto taxon = [&](qint64 id, std::optional<qint64> parent, const char *rank, const char *name) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.parentInatId = parent;
        t.rank = QLatin1String(rank);
        t.name = QString::fromUtf8(name);
        t.synonyms = {QString::fromUtf8(name) + QStringLiteral(" old")};
        QVERIFY(store.upsertTaxon(t) > 0);
    };
    taxon(47217, std::nullopt, "family", "Orchidaceae");
    taxon(100, 47217, "genus", "Diuris");
    taxon(900, 100, "species", "Diuris pardina");
    taxon(901, 100, "species", "Diuris × fastidiosa");
    const int pid = store.ensureProject(QStringLiteral("Orchids"), 47217, 7830, QStringLiteral("inat"));
    for (qint64 id : {47217, 100, 900, 901})
        store.addProjectTaxon(pid, id, id >= 900, false);
    store.addStatus(900, {7830, QStringLiteral("Rare"), QStringLiteral("VBA")});
    store.addOrigin(901, {std::nullopt, QStringLiteral("native"), QStringLiteral("APC")});

    exec(connection, QStringLiteral("INSERT INTO folder (id, path, parent_id, name, depth) VALUES "
                                    "(1, '%1', NULL, 'Nature Photos', 0), "
                                    "(2, '%1/Orchids', 1, 'Orchids', 1)")
                         .arg(root));
    exec(connection, QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on, "
                                    "latitude, longitude) VALUES "
                                    "(1, 2, 'IMG_1', '2024-08-06', -37.5, 144.25), "
                                    "(2, 2, 'IMG_2', NULL, NULL, NULL)"));
    exec(connection, QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES "
                                    "(1, '%1/Orchids/IMG_1.jpg', 'jpeg', 'jpg'), "
                                    "(1, '%1/Orchids/IMG_1.CR2', 'raw', 'cr2'), "
                                    "(2, '%1/Orchids/IMG_2.jpg', 'jpeg', 'jpg')")
                         .arg(root));
    exec(connection, QStringLiteral("INSERT INTO name_alias (raw_text, raw_folded, taxon_id, scope) "
                                    "VALUES ('Donkeys', 'donkeys', (SELECT id FROM taxon WHERE "
                                    "inat_id = 100), '%1/Orchids'), ('Leopard', 'leopard', "
                                    "(SELECT id FROM taxon WHERE inat_id = 900), 'global')")
                         .arg(root));
    exec(connection, QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, "
                                    "confidence, status) VALUES "
                                    "(1, (SELECT id FROM taxon WHERE inat_id = 900), 'folder', 0.93, 'auto')"));
}

void TestCatalogueSync::publishThenPullGivesTheSameCatalogue()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    populate(alice->connectionName(), kAliceRoot);

    CatalogueSync a = syncFor(*alice, kAliceRoot);
    const auto published = a.publish(base(QStringLiteral("alice")), kAlice);
    QVERIFY2(published, qPrintable(a.error()));
    QCOMPARE(published->newVersion, qint64(1));
    QCOMPARE(published->outsideRoot, 0);
    // Stored portably on the server.
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM rendition WHERE path LIKE 'lib:/Orchids/%'")),
             3);

    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    const auto pulled = b.pull();
    QVERIFY2(pulled, qPrintable(b.error()));
    QCOMPARE(*pulled, qint64(1));
    QCOMPARE(b.localVersion(), qint64(1));
    QCOMPARE(dump(bob->connectionName(), kBobRoot), dump(alice->connectionName(), kAliceRoot));
    // Bob's copy points into Bob's own library -- folder-scoped learned
    // names included.
    QCOMPARE(scalar(bob->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM folder WHERE path = '%1/Orchids'").arg(kBobRoot)),
             1);
    QCOMPARE(scalar(bob->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM name_alias WHERE scope = '%1/Orchids'").arg(kBobRoot)),
             1);
    QCOMPARE(scalar(bob->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM name_alias WHERE scope = 'global'")),
             1);
}

void TestCatalogueSync::pushSendsOnlyWhatChanged()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    const QString c = alice->connectionName();
    populate(c, kAliceRoot);
    CatalogueSync a = syncFor(*alice, kAliceRoot);
    QVERIFY2(a.publish(base(QStringLiteral("alice")), kAlice), qPrintable(a.error()));
    QCOMPARE(*a.pendingChanges(base(QStringLiteral("alice"))), 0);
    QCOMPARE(snapshotVersion(base(QStringLiteral("alice"))), qint64(1));

    // Inserts, an update, a delete, and a row replaced under a new id (as a
    // re-downloaded source does).
    taxonomy::TaxonomyStore store(c);
    taxonomy::Taxon t;
    t.inatId = 902;
    t.parentInatId = 100;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Diuris sulphurea");
    store.upsertTaxon(t);                                          // taxon + its name row: 2
    exec(c, QStringLiteral("UPDATE capture SET name_text = 'Leopard orchid' WHERE id = 2"));   // 1
    exec(c, QStringLiteral("DELETE FROM taxon_origin"));                                       // 1
    exec(c, QStringLiteral("UPDATE taxon_status SET id = id + 100"));   // old gone, new added: 2
    QCOMPARE(*a.pendingChanges(base(QStringLiteral("alice"))), 6);

    const auto pushed = a.push(base(QStringLiteral("alice")), kAlice);
    QVERIFY2(pushed, qPrintable(a.error()));
    QCOMPARE(pushed->changedRows, 6);
    QCOMPARE(pushed->newVersion, qint64(2));
    QCOMPARE(a.serverState()->pushedBy, QStringLiteral("Alice"));
    QVERIFY(!a.serverState()->pushedAt.isEmpty());
    QCOMPARE(*a.pendingChanges(base(QStringLiteral("alice"))), 0);   // snapshot refreshed
    // ...and it records the new version, since Discard copies it back.
    QCOMPARE(snapshotVersion(base(QStringLiteral("alice"))), qint64(2));

    // Nothing changed: nothing sent, the version stays.
    const auto again = a.push(base(QStringLiteral("alice")), kAlice);
    QVERIFY(again);
    QCOMPARE(again->changedRows, 0);
    QCOMPARE(a.serverState()->version, qint64(2));

    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    QVERIFY2(b.pull(), qPrintable(b.error()));
    QCOMPARE(dump(bob->connectionName(), kBobRoot), dump(c, kAliceRoot));
}

void TestCatalogueSync::pushKeepsForeignKeyOrder()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    const QString c = alice->connectionName();
    populate(c, kAliceRoot);
    CatalogueSync a = syncFor(*alice, kAliceRoot);
    QVERIFY2(a.publish(base(QStringLiteral("alice")), kAlice), qPrintable(a.error()));

    // Delete a taxon with names, tree links and a status (cascades locally);
    // add a new tree with its links; add a folder, a capture and a rendition.
    exec(c, QStringLiteral("DELETE FROM taxon WHERE inat_id = 900"));
    taxonomy::TaxonomyStore store(c);
    const int pid = store.ensureProject(QStringLiteral("Donkeys"), 100, std::nullopt,
                                        QStringLiteral("checklist"));
    store.addProjectTaxon(pid, 100, false, true);
    store.addProjectTaxon(pid, 901, true, true);
    exec(c, QStringLiteral("INSERT INTO folder (id, path, parent_id, name, depth) VALUES "
                           "(3, '%1/Orchids/New', 2, 'New', 2)").arg(kAliceRoot));
    exec(c, QStringLiteral("INSERT INTO capture (id, folder_id, base_name) VALUES (3, 3, 'IMG_3')"));
    exec(c, QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES "
                           "(3, '%1/Orchids/New/IMG_3.jpg', 'jpeg', 'jpg')").arg(kAliceRoot));

    QVERIFY2(a.push(base(QStringLiteral("alice")), kAlice), qPrintable(a.error()));
    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    QVERIFY2(b.pull(), qPrintable(b.error()));
    QCOMPARE(dump(bob->connectionName(), kBobRoot), dump(c, kAliceRoot));
}

void TestCatalogueSync::lockDecidesWhoMayPush()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    populate(alice->connectionName(), kAliceRoot);
    CatalogueSync a = syncFor(*alice, kAliceRoot);
    QVERIFY2(a.publish(base(QStringLiteral("alice")), kAlice), qPrintable(a.error()));
    QVERIFY(a.serverState()->heldBy(kAlice));   // publishing takes the lock

    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    QVERIFY(b.pull());
    QVERIFY(b.snapshotTo(base(QStringLiteral("bob"))));

    // Bob can't take the lock while Alice has it, nor push.
    QVERIFY(!b.acquireLock(kBob));
    QCOMPARE(b.serverState()->lockedBy, QStringLiteral("Alice"));
    exec(bob->connectionName(), QStringLiteral("UPDATE capture SET name_text = 'Bob was here'"));
    QVERIFY(!b.push(base(QStringLiteral("bob")), kBob));
    QVERIFY(b.error().contains(QStringLiteral("Alice is editing")));

    // Alice finishes; Bob takes the lock and pushes.
    QVERIFY(a.acquireLock(kAlice));   // again: still hers
    QVERIFY(a.releaseLock(kAlice));
    QVERIFY(!a.serverState()->isLocked());
    QVERIFY(b.acquireLock(kBob));
    QVERIFY2(b.push(base(QStringLiteral("bob")), kBob), qPrintable(b.error()));
    QCOMPARE(b.serverState()->version, qint64(2));

    // Alice breaks Bob's lock -- but her copy is from version 1, so her push
    // is refused until she gets the latest.
    QVERIFY(a.acquireLock(kAlice, true));
    exec(alice->connectionName(), QStringLiteral("UPDATE capture SET name_text = 'Alice'"));
    QVERIFY(!a.push(base(QStringLiteral("alice")), kAlice));
    QVERIFY(a.error().contains(QStringLiteral("changed after you last got it")));
}

void TestCatalogueSync::pathsOutsideTheRootAreKeptAndCounted()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    const QString c = alice->connectionName();
    populate(c, kAliceRoot);
    CatalogueSync a = syncFor(*alice, kAliceRoot);
    QVERIFY(a.publish(base(QStringLiteral("alice")), kAlice));

    exec(c, QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES "
                           "(2, '/mnt/usb/IMG_2.CR2', 'raw', 'cr2')"));
    const auto pushed = a.push(base(QStringLiteral("alice")), kAlice);
    QVERIFY2(pushed, qPrintable(a.error()));
    QCOMPARE(pushed->outsideRoot, 1);
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM rendition WHERE path = '/mnt/usb/IMG_2.CR2'")),
             1);
}

// The library root moved to another folder of the catalogue after the last
// sync: unchanged rows are re-sent under the new root, and the old root
// folder's "lib:/" never collides with the new one's.
void TestCatalogueSync::changingTheRootResendsMovedPaths()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    auto alice = openLocal(QStringLiteral("alice"));
    const QString c = alice->connectionName();
    populate(c, kAliceRoot);
    const QString oldRoot = QStringLiteral("/mnt/drive/Old Sync");
    exec(c, QStringLiteral("INSERT INTO folder (id, path, parent_id, name, depth) VALUES "
                           "(10, '%1', NULL, 'Old Sync', 0)").arg(oldRoot));
    {
        CatalogueSync before = syncFor(*alice, oldRoot);
        QVERIFY2(before.publish(base(QStringLiteral("alice")), kAlice), qPrintable(before.error()));
    }
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT id FROM folder WHERE path = 'lib:/'")), 10);

    CatalogueSync a = syncFor(*alice, kAliceRoot);
    QCOMPARE(*a.pendingChanges(base(QStringLiteral("alice"))), 7);   // 3 folders, 3 renditions, an alias
    const auto pushed = a.push(base(QStringLiteral("alice")), kAlice);
    QVERIFY2(pushed, qPrintable(a.error()));
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT id FROM folder WHERE path = 'lib:/'")), 1);
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM folder WHERE path = '%1'").arg(oldRoot)), 1);
    QCOMPARE(scalar(m_server->connectionName(),
                    QStringLiteral("SELECT COUNT(*) FROM rendition WHERE path LIKE 'lib:/Orchids/%'")),
             3);
    QCOMPARE(*a.pendingChanges(base(QStringLiteral("alice"))), 0);   // the new root is recorded

    // Removing the old root folder now is an ordinary change.
    exec(c, QStringLiteral("DELETE FROM folder WHERE id = 10"));
    QVERIFY2(a.push(base(QStringLiteral("alice")), kAlice), qPrintable(a.error()));

    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    QVERIFY2(b.pull(), qPrintable(b.error()));
    QCOMPARE(dump(bob->connectionName(), kBobRoot), dump(c, kAliceRoot));
}

void TestCatalogueSync::pullRefusesAnotherSchemaVersion()
{
    if (!openServer())
        QSKIP("set PHOTOLIFE_TEST_PG_DSN to run against Postgres");
    exec(m_server->connectionName(),
         QStringLiteral("INSERT INTO schema_migrations (version, applied_at) VALUES (999, 'x')"));
    auto bob = openLocal(QStringLiteral("bob"));
    CatalogueSync b = syncFor(*bob, kBobRoot);
    QVERIFY(!b.pull());
    QVERIFY(b.error().contains(QStringLiteral("newer PhotoLife")));
    exec(m_server->connectionName(), QStringLiteral("DELETE FROM schema_migrations WHERE version = 999"));
}

void TestCatalogueSync::pathMappingRoundTrips()
{
    const PathMapping win(QStringLiteral("D:/Pictures/Nature/"));
    QCOMPARE(win.libraryRoot(), QStringLiteral("D:/Pictures/Nature"));
    QCOMPARE(win.toServer(QStringLiteral("D:/Pictures/Nature/Orchids/a.jpg")),
             QStringLiteral("lib:/Orchids/a.jpg"));
#ifdef Q_OS_WIN
    // Native separators, and Windows paths compare case-insensitively.
    QCOMPARE(PathMapping(QStringLiteral("D:\\Pictures\\Nature\\")).libraryRoot(),
             QStringLiteral("D:/Pictures/Nature"));
    QCOMPARE(win.toServer(QStringLiteral("d:\\pictures\\nature\\Orchids\\a.jpg")),
             QStringLiteral("lib:/Orchids/a.jpg"));
#endif
    QCOMPARE(win.toServer(QStringLiteral("D:/Pictures/Nature")), QStringLiteral("lib:/"));
    bool outside = false;
    QCOMPARE(win.toServer(QStringLiteral("D:/Pictures/Naturefake/a.jpg"), &outside),
             QStringLiteral("D:/Pictures/Naturefake/a.jpg"));
    QVERIFY(outside);

    const PathMapping linux(QStringLiteral("/home/bob/Photos"));
    QCOMPARE(linux.toLocal(QStringLiteral("lib:/Orchids/a.jpg")),
             QStringLiteral("/home/bob/Photos/Orchids/a.jpg"));
    QCOMPARE(linux.toLocal(QStringLiteral("lib:/")), QStringLiteral("/home/bob/Photos"));
    QCOMPARE(linux.toLocal(QStringLiteral("/mnt/usb/x.jpg")), QStringLiteral("/mnt/usb/x.jpg"));
    QCOMPARE(PathMapping().toServer(QStringLiteral("/a/b")), QStringLiteral("/a/b"));   // no root: as is
}

// A downloaded catalogue replaces the local one only at the next start --
// and the one it replaces is kept, WAL file included, newest few only.
void TestCatalogueSync::applyingAPullKeepsBackups()
{
    QTemporaryDir dir;
    const QString db = dir.filePath(QStringLiteral("catalogue.db"));
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };

    QVERIFY(!sync::SyncService::applyPendingPull(db));   // nothing pending
    write(db, "mine");
    write(db + QStringLiteral("-wal"), "unsaved");
    write(sync::SyncService::pulledPath(db), "server 1");
    QVERIFY(!sync::SyncService::applyPendingPull(db));   // no snapshot: an interrupted pull
    QCOMPARE(read(db), QByteArray("mine"));

    for (int i = 1; i <= 4; ++i) {
        write(sync::SyncService::pulledPath(db), "server " + QByteArray::number(i));
        write(sync::SyncService::basePath(sync::SyncService::pulledPath(db)), "base");
        QVERIFY(sync::SyncService::applyPendingPull(db));
        QCOMPARE(read(db), "server " + QByteArray::number(i));
        QCOMPARE(read(sync::SyncService::basePath(db)), QByteArray("base"));
    }
    const QStringList backups =
        QDir(dir.path()).entryList({QStringLiteral("catalogue.before-sync-*.db")}, QDir::Files,
                                   QDir::Name);
    QCOMPARE(backups.size(), sync::SyncService::kBackupsKept);
    // The oldest (the original) was pruned; the newest kept is "server 3".
    QCOMPARE(read(dir.filePath(backups.last())), QByteArray("server 3"));
    QVERIFY(!QFile::exists(sync::SyncService::pulledPath(db)));
}

namespace {

sync::SyncService::Outcome runAndWait(sync::SyncService &service, sync::SyncService::Op op,
                                      const QString &restoreFrom = {})
{
    QSignalSpy spy(&service, &sync::SyncService::finished);
    if (op == sync::SyncService::Op::RestoreBackup)
        service.restoreBackup(restoreFrom);
    else
        service.run(op);
    if (spy.isEmpty())
        spy.wait(10000);
    return spy.isEmpty() ? sync::SyncService::Outcome{}
                         : spy.at(0).at(0).value<sync::SyncService::Outcome>();
}

} // namespace

void TestCatalogueSync::discardGoesBackToTheLastSync()
{
    const QString path = m_dir->filePath(QStringLiteral("catalogue.db"));
    {
        auto db = std::make_unique<Database>();
        QVERIFY(db->open(path));
        populate(db->connectionName(), kAliceRoot);
        CatalogueSync s(db->connectionName(), QString(), PathMapping());
        QVERIFY(s.snapshotTo(sync::SyncService::basePath(path)));
        exec(db->connectionName(), QStringLiteral("DELETE FROM capture_match"));   // an edit
    }
    sync::SyncService service;
    service.setConfig({path, {}, kAlice, kAliceRoot});
    const auto changes = runAndWait(service, sync::SyncService::Op::Changes);
    QVERIFY2(changes.ok, qPrintable(changes.error));
    QCOMPARE(changes.pending, 1);

    const auto discard = runAndWait(service, sync::SyncService::Op::Discard);
    QVERIFY2(discard.ok && discard.pulled, qPrintable(discard.error));
    QVERIFY(sync::SyncService::applyPendingPull(path));   // what the restart does
    auto db = std::make_unique<Database>();
    QVERIFY(db->open(path));
    QCOMPARE(scalar(db->connectionName(), QStringLiteral("SELECT COUNT(*) FROM capture_match")), 1);
    QCOMPARE(service.backups().size(), 1);   // the discarded version is kept
}

void TestCatalogueSync::restoreKeepsTheSyncPosition()
{
    const QString path = m_dir->filePath(QStringLiteral("catalogue.db"));
    const QString backup = m_dir->filePath(QStringLiteral("catalogue.before-sync-20260101-000000-01.db"));
    {
        auto db = std::make_unique<Database>();
        QVERIFY(db->open(path));
        populate(db->connectionName(), kAliceRoot);
        CatalogueSync s(db->connectionName(), QString(), PathMapping());
        QVERIFY(s.snapshotTo(backup));   // an older copy, with the match...
        exec(db->connectionName(), QStringLiteral("DELETE FROM capture_match"));
        exec(db->connectionName(), QStringLiteral("UPDATE catalogue_sync SET version = 7"));
        QVERIFY(s.snapshotTo(sync::SyncService::basePath(path)));   // ...synced without it
    }
    sync::SyncService service;
    service.setConfig({path, {}, kAlice, kAliceRoot});
    const auto restore = runAndWait(service, sync::SyncService::Op::RestoreBackup, backup);
    QVERIFY2(restore.ok && restore.pulled, qPrintable(restore.error));
    QVERIFY(sync::SyncService::applyPendingPull(path));

    auto db = std::make_unique<Database>();
    QVERIFY(db->open(path));
    QCOMPARE(scalar(db->connectionName(), QStringLiteral("SELECT COUNT(*) FROM capture_match")), 1);
    // Same sync position as before, so the restored match is a change to send.
    QCOMPARE(scalar(db->connectionName(), QStringLiteral("SELECT version FROM catalogue_sync")), 7);
    CatalogueSync s(db->connectionName(), QString(), PathMapping());
    QCOMPARE(*s.pendingChanges(sync::SyncService::basePath(path)), 1);
}

QTEST_GUILESS_MAIN(TestCatalogueSync)
#include "tst_cataloguesync.moc"
