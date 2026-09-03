#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"

using namespace pl;
using namespace pl::scan;

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(bytes);
}

} // namespace

class TestCatalogueWriter : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void writesFolderTreeCaptureAndRenditions();
    void secondSyncLeavesUnchangedFilesAlone();
    void changedFileIsRehashed();
    void cancelledSyncCommitsNothing();

private:
    QTemporaryDir m_tmp;
    QString m_root;
    std::unique_ptr<Database> m_db;

    int scalarInt(const QString &sql);
    QSqlQuery exec(const QString &sql);
};

void TestCatalogueWriter::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Flora Photos"));

    writeFile(m_root + QStringLiteral("/Orchidaceae/Diuris/Diuris pardina/"
                                      "Diuris pardina (bud) - Dadswells Bridge 28-9-2020 (1).jpg"),
              QByteArrayLiteral("jpeg-bytes-one"));
    writeFile(m_root + QStringLiteral("/Orchidaceae/Diuris/Diuris pardina/"
                                      "Diuris pardina (bud) - Dadswells Bridge 28-9-2020 (1).nef"),
              QByteArrayLiteral("raw-bytes-one"));
    writeFile(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                      "Caladenia carnea - Anglesea 3-10-2019.jpg"),
              QByteArrayLiteral("jpeg-bytes-two"));

    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
}

void TestCatalogueWriter::cleanup()
{
    m_db.reset();
}

QSqlQuery TestCatalogueWriter::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    if (!q.exec(sql))
        qWarning() << "query failed:" << q.lastError().text() << sql;
    return q;
}

int TestCatalogueWriter::scalarInt(const QString &sql)
{
    QSqlQuery q = exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

void TestCatalogueWriter::writesFolderTreeCaptureAndRenditions()
{
    FileScanner scanner;
    const auto caps = scanner.scan({m_root});

    CatalogueWriter writer(m_db->connectionName());
    const ScanSummary s = writer.sync(caps, {m_root});

    QVERIFY2(s.ok(), qPrintable(s.error));
    QCOMPARE(s.capturesAdded, 2);
    QCOMPARE(s.renditionsAdded, 3);
    QCOMPARE(s.filesHashed, 3);

    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM capture")), 2);
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM rendition")), 3);

    // Folder tree: root at depth 0, "Diuris pardina" at depth 3, linked to "Diuris".
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT depth FROM folder WHERE name = 'Flora Photos'")), 0);
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT depth FROM folder WHERE name = 'Diuris pardina'")), 3);
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT f.depth FROM folder f JOIN folder p ON f.parent_id = p.id "
                 "WHERE f.name = 'Diuris pardina' AND p.name = 'Diuris'")), 3);

    // Parsed fields land on the capture.
    QSqlQuery q = exec(QStringLiteral(
        "SELECT name_text, locality_text, organ_tags, captured_on, date_source "
        "FROM capture WHERE base_name LIKE 'Diuris pardina%'"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("Diuris pardina"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("Dadswells Bridge"));
    QCOMPARE(q.value(2).toString(), QStringLiteral("bud"));
    QCOMPARE(q.value(3).toString(), QStringLiteral("2020-09-28"));
    QCOMPARE(q.value(4).toString(), QStringLiteral("filename"));

    // RAW + JPEG are two renditions of the one capture.
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT COUNT(*) FROM rendition r JOIN capture c ON r.capture_id = c.id "
                 "WHERE c.base_name LIKE 'Diuris pardina%'")), 2);
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT COUNT(DISTINCT content_hash) FROM rendition")), 3);
}

void TestCatalogueWriter::secondSyncLeavesUnchangedFilesAlone()
{
    FileScanner scanner;
    const auto caps = scanner.scan({m_root});

    CatalogueWriter first(m_db->connectionName());
    first.sync(caps, {m_root});

    CatalogueWriter second(m_db->connectionName());
    const ScanSummary s = second.sync(scanner.scan({m_root}), {m_root});

    QVERIFY2(s.ok(), qPrintable(s.error));
    QCOMPARE(s.capturesAdded, 0);
    QCOMPARE(s.renditionsAdded, 0);
    QCOMPARE(s.renditionsUnchanged, 3);
    QCOMPARE(s.filesHashed, 0);
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM rendition")), 3);
}

void TestCatalogueWriter::changedFileIsRehashed()
{
    FileScanner scanner;
    CatalogueWriter first(m_db->connectionName());
    first.sync(scanner.scan({m_root}), {m_root});

    const QString changed = m_root + QStringLiteral(
        "/Orchidaceae/Caladenia/Caladenia carnea/Caladenia carnea - Anglesea 3-10-2019.jpg");
    writeFile(changed, QByteArrayLiteral("jpeg-bytes-two-EDITED-and-longer"));

    CatalogueWriter second(m_db->connectionName());
    const ScanSummary s = second.sync(scanner.scan({m_root}), {m_root});

    QVERIFY2(s.ok(), qPrintable(s.error));
    QCOMPARE(s.renditionsUpdated, 1);
    QCOMPARE(s.renditionsUnchanged, 2);
    QCOMPARE(s.filesHashed, 1);
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM rendition")), 3);
}

void TestCatalogueWriter::cancelledSyncCommitsNothing()
{
    FileScanner scanner;
    const auto caps = scanner.scan({m_root});

    CatalogueWriter writer(m_db->connectionName());
    writer.setCancelPredicate([] { return true; });
    const ScanSummary s = writer.sync(caps, {m_root});

    QVERIFY(s.cancelled);
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM capture")), 0);
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM folder")), 0);
}

QTEST_GUILESS_MAIN(TestCatalogueWriter)
#include "tst_cataloguewriter.moc"
