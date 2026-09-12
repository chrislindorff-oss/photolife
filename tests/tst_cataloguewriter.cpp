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

// --- Minimal JPEG-with-GPS-EXIF builder, mirroring tst_exif.cpp's helpers
// (duplicated here since those are file-local, not exported). Only enough to
// exercise CatalogueWriter's GPS write-back — IFD-parsing edge cases are
// already covered by tst_exif.cpp.

void putU16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}

void putU32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

void putEntry(QByteArray &b, quint16 tag, quint16 type, quint32 count, quint32 value)
{
    putU16(b, tag);
    putU16(b, type);
    putU32(b, count);
    putU32(b, value);
}

void putEntryInlineAscii(QByteArray &b, quint16 tag, const QByteArray &text)
{
    putU16(b, tag);
    putU16(b, 2);   // ASCII
    putU32(b, quint32(text.size()));
    QByteArray padded = text;
    while (padded.size() < 4)
        padded.append('\0');
    b.append(padded.left(4));
}

void putRational(QByteArray &b, quint32 num, quint32 den)
{
    putU32(b, num);
    putU32(b, den);
}

// A little-endian TIFF blob (the bytes that follow "Exif\0\0"): IFD0 points at
// a GPS IFD carrying GPSLatitude(Ref)/GPSLongitude(Ref) — 37.5S, 145.25E.
QByteArray tiffWithGps()
{
    const quint32 kGpsIfdAt = 8 + 2 + 1 * 12 + 4;    // IFD0: 1 entry
    const quint32 kGpsLatAt = kGpsIfdAt + 2 + 4 * 12 + 4;
    const quint32 kGpsLonAt = kGpsLatAt + 24;

    QByteArray b;
    b.append("II");
    putU16(b, 0x002A);
    putU32(b, 8);                       // IFD0 at offset 8

    putU16(b, 1);                       // IFD0: 1 entry
    putEntry(b, 0x8825, 4, 1, kGpsIfdAt);   // GPSInfo IFD pointer
    putU32(b, 0);                       // no IFD1
    Q_ASSERT(b.size() == int(kGpsIfdAt));

    putU16(b, 4);                                       // GPS IFD: 4 entries
    putEntryInlineAscii(b, 0x0001, "S");                // GPSLatitudeRef
    putEntry(b, 0x0002, 5, 3, kGpsLatAt);               // GPSLatitude
    putEntryInlineAscii(b, 0x0003, "E");                // GPSLongitudeRef
    putEntry(b, 0x0004, 5, 3, kGpsLonAt);               // GPSLongitude
    putU32(b, 0);
    Q_ASSERT(b.size() == int(kGpsLatAt));
    putRational(b, 37, 1);
    putRational(b, 30, 1);
    putRational(b, 0, 1);    // -> 37.5 degrees
    Q_ASSERT(b.size() == int(kGpsLonAt));
    putRational(b, 145, 1);
    putRational(b, 15, 1);
    putRational(b, 0, 1);    // -> 145.25 degrees

    return b;
}

QByteArray jpegWithGps()
{
    const QByteArray payload = QByteArrayLiteral("Exif\0\0") + tiffWithGps();
    QByteArray jpeg;
    jpeg.append('\xFF').append('\xD8');                   // SOI
    jpeg.append('\xFF').append('\xE1');                   // APP1
    const int segLen = payload.size() + 2;
    jpeg.append(char((segLen >> 8) & 0xFF)).append(char(segLen & 0xFF));   // big-endian
    jpeg.append(payload);
    jpeg.append('\xFF').append('\xD9');                   // EOI
    return jpeg;
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
    void renamedFileKeepsCaptureIdentity();
    void cancelledSyncCommitsNothing();
    void writesGpsFromExif();
    void unchangedRescanKeepsExistingGps();
    void backfillsGpsForPreExistingUncheckedRendition();

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

void TestCatalogueWriter::renamedFileKeepsCaptureIdentity()
{
    const QString oldPath = m_root + QStringLiteral(
        "/Orchidaceae/Caladenia/Caladenia carnea/Caladenia carnea - Anglesea 3-10-2019.jpg");
    FileScanner scanner;
    CatalogueWriter first(m_db->connectionName());
    QVERIFY(first.sync(scanner.scan({m_root}), {m_root}).ok());

    const int oldCaptureId = scalarInt(QStringLiteral(
        "SELECT c.id FROM capture c JOIN rendition r ON r.capture_id = c.id "
        "WHERE r.path LIKE '%3-10-2019.jpg'"));
    const QString newPath = m_root + QStringLiteral(
        "/Orchidaceae/Caladenia/Caladenia carnea/renamed-photo.jpg");
    QVERIFY(QFile::rename(oldPath, newPath));

    CatalogueWriter second(m_db->connectionName());
    const ScanSummary summary = second.sync(scanner.scan({m_root}), {m_root});
    QVERIFY2(summary.ok(), qPrintable(summary.error));
    QCOMPARE(scalarInt(QStringLiteral("SELECT COUNT(*) FROM capture")), 2);
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT c.id FROM capture c JOIN rendition r ON r.capture_id = c.id "
                 "WHERE r.path LIKE '%renamed-photo.jpg'")),
             oldCaptureId);
    QCOMPARE(scalarInt(QStringLiteral(
                 "SELECT COUNT(*) FROM rendition WHERE path LIKE '%3-10-2019.jpg'")),
             0);
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

void TestCatalogueWriter::writesGpsFromExif()
{
    const QString path = m_root + QStringLiteral(
        "/Orchidaceae/Diuris/Diuris pardina/GPS test - Loc 1-1-2021.jpg");
    writeFile(path, jpegWithGps());

    FileScanner scanner;
    CatalogueWriter writer(m_db->connectionName());
    const ScanSummary s = writer.sync(scanner.scan({m_root}), {m_root});
    QVERIFY2(s.ok(), qPrintable(s.error));

    QSqlQuery q = exec(QStringLiteral(
        "SELECT latitude, longitude FROM capture WHERE base_name LIKE 'GPS test%'"));
    QVERIFY(q.next());
    QVERIFY(!q.value(0).isNull());
    QVERIFY(!q.value(1).isNull());
    QVERIFY(qAbs(q.value(0).toDouble() - (-37.5)) < 1e-6);
    QVERIFY(qAbs(q.value(1).toDouble() - 145.25) < 1e-6);
}

void TestCatalogueWriter::unchangedRescanKeepsExistingGps()
{
    const QString path = m_root + QStringLiteral(
        "/Orchidaceae/Diuris/Diuris pardina/GPS test - Loc 1-1-2021.jpg");
    writeFile(path, jpegWithGps());

    FileScanner scanner;
    CatalogueWriter first(m_db->connectionName());
    QVERIFY(first.sync(scanner.scan({m_root}), {m_root}).ok());

    CatalogueWriter second(m_db->connectionName());
    const ScanSummary s = second.sync(scanner.scan({m_root}), {m_root});
    QVERIFY2(s.ok(), qPrintable(s.error));
    QCOMPARE(s.renditionsAdded, 0);   // nothing changed on this pass

    QSqlQuery q = exec(QStringLiteral(
        "SELECT latitude, longitude FROM capture WHERE base_name LIKE 'GPS test%'"));
    QVERIFY(q.next());
    QVERIFY(!q.value(0).isNull());
    QVERIFY(!q.value(1).isNull());
}

void TestCatalogueWriter::backfillsGpsForPreExistingUncheckedRendition()
{
    // Simulate a capture that was already in the catalogue *before* GPS
    // support existed: geo_checked defaults to 0 for every pre-existing row
    // (that's what the migration does), and its coordinates are unset, even
    // though the file on disk has always carried GPS EXIF.
    const QString path = m_root + QStringLiteral(
        "/Orchidaceae/Diuris/Diuris pardina/GPS test - Loc 1-1-2021.jpg");
    writeFile(path, jpegWithGps());

    FileScanner scanner;
    CatalogueWriter first(m_db->connectionName());
    QVERIFY(first.sync(scanner.scan({m_root}), {m_root}).ok());

    exec(QStringLiteral(
        "UPDATE rendition SET geo_checked = 0 WHERE path LIKE '%GPS test%'"));
    exec(QStringLiteral(
        "UPDATE capture SET latitude = NULL, longitude = NULL "
        "WHERE base_name LIKE 'GPS test%'"));

    // The file itself has NOT changed (same size/mtime) — only geo_checked
    // was reset — so this must still trigger a backfill, not be skipped as
    // "unchanged".
    CatalogueWriter second(m_db->connectionName());
    const ScanSummary s = second.sync(scanner.scan({m_root}), {m_root});
    QVERIFY2(s.ok(), qPrintable(s.error));

    QSqlQuery geo = exec(QStringLiteral(
        "SELECT geo_checked FROM rendition WHERE path LIKE '%GPS test%'"));
    QVERIFY(geo.next());
    QCOMPARE(geo.value(0).toInt(), 1);

    QSqlQuery q = exec(QStringLiteral(
        "SELECT latitude, longitude FROM capture WHERE base_name LIKE 'GPS test%'"));
    QVERIFY(q.next());
    QVERIFY(!q.value(0).isNull());
    QVERIFY(!q.value(1).isNull());
    QVERIFY(qAbs(q.value(0).toDouble() - (-37.5)) < 1e-6);
    QVERIFY(qAbs(q.value(1).toDouble() - 145.25) < 1e-6);
}

QTEST_GUILESS_MAIN(TestCatalogueWriter)
#include "tst_cataloguewriter.moc"
