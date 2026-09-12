#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CatalogueMaintenance.h"

using namespace pl;
using namespace pl::scan;

class TestCatalogueMaintenance : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void forgetCapturesCascadesRenditionsAndMatches();
    void forgetCapturesIgnoresUnknownIds();
    void capturesWithMissingFilesFindsGhostsOnly();
    void backfillIgnoresNonExistentRawFiles();
    void backfillSkipsCapturesWithoutRawRendition();

private:
    QTemporaryDir m_tmp;
    std::unique_ptr<Database> m_db;

    int addFolder(const QString &path);
    int addCapture(int folderId, const QString &baseName);
    int addRendition(int captureId, const QString &path);
    int addRawRendition(int captureId, const QString &path);
    int count(const QString &table) const;
    QString touch(const QString &rel);   // creates a file, returns its absolute path
};

void TestCatalogueMaintenance::init()
{
    QVERIFY(m_tmp.isValid());
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
}

void TestCatalogueMaintenance::cleanup()
{
    m_db.reset();
}

int TestCatalogueMaintenance::addFolder(const QString &path)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES (?, ?, 0)"));
    q.addBindValue(path);
    q.addBindValue(QFileInfo(path).fileName());
    const bool ok = q.exec();
    Q_ASSERT(ok);
    Q_UNUSED(ok);
    return q.lastInsertId().toInt();
}

int TestCatalogueMaintenance::addCapture(int folderId, const QString &baseName)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (?, ?)"));
    q.addBindValue(folderId);
    q.addBindValue(baseName);
    const bool ok = q.exec();
    Q_ASSERT(ok);
    Q_UNUSED(ok);
    return q.lastInsertId().toInt();
}

int TestCatalogueMaintenance::addRendition(int captureId, const QString &path)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    q.addBindValue(captureId);
    q.addBindValue(path);
    const bool ok = q.exec();
    Q_ASSERT(ok);
    Q_UNUSED(ok);
    return q.lastInsertId().toInt();
}

int TestCatalogueMaintenance::addRawRendition(int captureId, const QString &path)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'raw', 'nef')"));
    q.addBindValue(captureId);
    q.addBindValue(path);
    const bool ok = q.exec();
    Q_ASSERT(ok);
    Q_UNUSED(ok);
    return q.lastInsertId().toInt();
}

int TestCatalogueMaintenance::count(const QString &table) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table);
    q.next();
    return q.value(0).toInt();
}

QString TestCatalogueMaintenance::touch(const QString &rel)
{
    const QString path = m_tmp.filePath(rel);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("x");
    return path;
}

void TestCatalogueMaintenance::forgetCapturesCascadesRenditionsAndMatches()
{
    const int folder = addFolder(m_tmp.filePath(QStringLiteral("Lib")));
    const int keep = addCapture(folder, QStringLiteral("keep"));
    const int drop = addCapture(folder, QStringLiteral("drop"));
    addRendition(keep, m_tmp.filePath(QStringLiteral("Lib/keep.jpg")));
    addRendition(drop, m_tmp.filePath(QStringLiteral("Lib/drop.jpg")));
    addRendition(drop, m_tmp.filePath(QStringLiteral("Lib/drop.cr2")));

    QSqlQuery m(QSqlDatabase::database(m_db->connectionName(), false));
    m.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, method, status) VALUES (?, 'test', 'pending')"));
    m.addBindValue(drop);
    QVERIFY(m.exec());

    CatalogueMaintenance maint(m_db->connectionName());
    QCOMPARE(maint.forgetCaptures({drop}), 1);

    QCOMPARE(count(QStringLiteral("capture")), 1);
    QCOMPARE(count(QStringLiteral("rendition")), 1);      // both of drop's renditions gone
    QCOMPARE(count(QStringLiteral("capture_match")), 0);  // cascaded

    QSqlQuery left(QSqlDatabase::database(m_db->connectionName(), false));
    left.exec(QStringLiteral("SELECT id FROM capture"));
    QVERIFY(left.next());
    QCOMPARE(left.value(0).toInt(), keep);
}

void TestCatalogueMaintenance::forgetCapturesIgnoresUnknownIds()
{
    const int folder = addFolder(m_tmp.filePath(QStringLiteral("Lib")));
    const int only = addCapture(folder, QStringLiteral("only"));

    CatalogueMaintenance maint(m_db->connectionName());
    QCOMPARE(maint.forgetCaptures({}), 0);
    QCOMPARE(maint.forgetCaptures({999999}), 0);
    QCOMPARE(count(QStringLiteral("capture")), 1);

    QCOMPARE(maint.forgetCaptures({only, 999999}), 1);   // the real one still goes
    QCOMPARE(count(QStringLiteral("capture")), 0);
}

void TestCatalogueMaintenance::capturesWithMissingFilesFindsGhostsOnly()
{
    const int folder = addFolder(m_tmp.filePath(QStringLiteral("Lib")));

    const QString presentPath = touch(QStringLiteral("Lib/present.jpg"));
    const int present = addCapture(folder, QStringLiteral("present"));
    addRendition(present, presentPath);

    // A capture whose RAW is gone but whose JPEG is still there is NOT a ghost.
    const QString partialJpeg = touch(QStringLiteral("Lib/partial.jpg"));
    const int partial = addCapture(folder, QStringLiteral("partial"));
    addRendition(partial, partialJpeg);
    addRendition(partial, m_tmp.filePath(QStringLiteral("Lib/partial.cr2")));

    // Every file gone -> a ghost.
    const int ghost = addCapture(folder, QStringLiteral("ghost"));
    addRendition(ghost, m_tmp.filePath(QStringLiteral("Lib/ghost-old-name.jpg")));

    CatalogueMaintenance maint(m_db->connectionName());
    const auto missing = maint.capturesWithMissingFiles();

    QList<int> ids;
    for (const auto &mc : missing)
        ids << mc.captureId;
    QVERIFY(!ids.contains(present));   // its file is on disk
    QVERIFY(!ids.contains(partial));   // one of its two files is on disk
    QCOMPARE(ids, QList<int>{ghost});

    // The entry carries what to show the user.
    QCOMPARE(missing.first().baseName, QStringLiteral("ghost"));
    QCOMPARE(missing.first().missingPaths.size(), 1);
    QVERIFY(missing.first().missingPaths.first().endsWith(QStringLiteral("ghost-old-name.jpg")));

    QCOMPARE(maint.forgetCaptures(ids), 1);
    QCOMPARE(count(QStringLiteral("capture")), 2);
}

void TestCatalogueMaintenance::backfillIgnoresNonExistentRawFiles()
{
    const int folder = addFolder(m_tmp.filePath(QStringLiteral("Lib")));
    const int cap = addCapture(folder, QStringLiteral("orphan"));
    addRawRendition(cap, m_tmp.filePath(QStringLiteral("Lib/does-not-exist.nef")));

    CatalogueMaintenance maint(m_db->connectionName());
    QCOMPARE(maint.backfillRawGeolocation(), 0);
    QVERIFY(maint.error().isEmpty());

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT latitude, longitude FROM capture WHERE id = ?"));
    q.addBindValue(cap);
    QVERIFY(q.exec());
    QVERIFY(q.next());
    QVERIFY(q.value(0).isNull());
    QVERIFY(q.value(1).isNull());
}

void TestCatalogueMaintenance::backfillSkipsCapturesWithoutRawRendition()
{
    const int folder = addFolder(m_tmp.filePath(QStringLiteral("Lib")));
    const int cap = addCapture(folder, QStringLiteral("jpegonly"));
    addRendition(cap, touch(QStringLiteral("Lib/jpegonly.jpg")));

    CatalogueMaintenance maint(m_db->connectionName());
    QCOMPARE(maint.backfillRawGeolocation(), 0);
    QVERIFY(maint.error().isEmpty());
}

QTEST_GUILESS_MAIN(TestCatalogueMaintenance)
#include "tst_cataloguemaintenance.moc"
