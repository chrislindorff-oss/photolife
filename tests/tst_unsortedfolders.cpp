#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "scan/UnsortedFolders.h"

using namespace pl;
using namespace pl::scan;

namespace {

void putU16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF));
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

// A JPEG whose EXIF has only DateTimeOriginal; `salt` makes each file unique.
QByteArray jpegTaken(const QDateTime &when, int salt)
{
    const quint32 kExifIfdAt = 8 + 2 + 12 + 4;
    const quint32 kDateAt = kExifIfdAt + 2 + 12 + 4;
    QByteArray b;
    b.append("II");
    putU16(b, 0x002A);
    putU32(b, 8);
    putU16(b, 1);
    putEntry(b, 0x8769, 4, 1, kExifIfdAt);
    putU32(b, 0);
    putU16(b, 1);
    putEntry(b, 0x9003, 2, 20, kDateAt);
    putU32(b, 0);
    QByteArray date = when.toString(QStringLiteral("yyyy:MM:dd HH:mm:ss")).toLatin1();
    date.append('\0');
    b.append(date);
    const QByteArray payload = QByteArrayLiteral("Exif\0\0") + b;
    QByteArray jpeg("\xFF\xD8\xFF\xE1", 4);
    const int segLen = int(payload.size()) + 2;
    jpeg.append(char((segLen >> 8) & 0xFF)).append(char(segLen & 0xFF));
    jpeg.append(payload);
    jpeg.append("\xFF\xD9", 2);
    jpeg.append(QByteArray::number(salt));
    return jpeg;
}

} // namespace

class TestUnsortedFolders : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void runsAreShotsOfOneSubjectSecondsApart();
    void markingKeepsOnePerRunAndWhatYouDecided();
    void rescansSkipRememberedShotsAndAddNewRuns();
    void cullingRemovesAndPromotes();
    void unmarkingCataloguesEverythingAgain();

private:
    void shoot(const QString &name, const QTime &at, int n = 1);
    ScanSummary scan();
    int count(const QString &sql) const;

    QTemporaryDir m_tmp;
    QString m_root, m_osprey;
    std::unique_ptr<Database> m_db;
    int m_salt = 0;
};

void TestUnsortedFolders::init()
{
    QVERIFY(m_tmp.isValid());
    m_salt = 0;
    m_root = m_tmp.filePath(QStringLiteral("Lib"));
    m_osprey = m_root + QStringLiteral("/To Sort/Osprey");
    QDir().mkpath(m_osprey);
    QDir().mkpath(m_root + QStringLiteral("/Sorted"));
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(m_tmp.filePath(QStringLiteral("catalogue.db"))));
}

void TestUnsortedFolders::cleanup()
{
    m_db.reset();
    QDir(m_root).removeRecursively();
    QFile::remove(m_tmp.filePath(QStringLiteral("catalogue.db")));
}

// `n` shots, a second apart, from `at`.
void TestUnsortedFolders::shoot(const QString &name, const QTime &at, int n)
{
    for (int i = 0; i < n; ++i) {
        QFile f(m_osprey + QStringLiteral("/%1 (%2).jpg").arg(name).arg(++m_salt));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(jpegTaken(QDateTime(QDate(2026, 6, 4), at.addSecs(i)), m_salt));
    }
}

ScanSummary TestUnsortedFolders::scan()
{
    FileScanner scanner;
    return CatalogueWriter(m_db->connectionName()).sync(scanner.scan({m_root}), {m_root});
}

int TestUnsortedFolders::count(const QString &sql) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    return q.exec(sql) && q.next() ? q.value(0).toInt() : -1;
}

void TestUnsortedFolders::runsAreShotsOfOneSubjectSecondsApart()
{
    const QDateTime t(QDate(2026, 6, 4), QTime(10, 0, 0));
    const QList<RunItem> items{
        {QStringLiteral("a"), t, t.date()},
        {QStringLiteral("a"), t.addSecs(10), t.date()},    // 10 s: same run
        {QStringLiteral("a"), t.addSecs(21), t.date()},    // 11 s later: a new run
        {QStringLiteral("b"), t.addSecs(1), t.date()},     // another subject
        {QStringLiteral("a"), {}, t.date()},               // undated: by day
        {QStringLiteral("a"), {}, t.date()},
        {QStringLiteral("a"), {}, t.date().addDays(1)},
    };
    const QList<QList<int>> runs = groupRuns(items);
    QCOMPARE(runs.size(), 5);
    QCOMPARE(runs.at(0), (QList<int>{0, 1}));
    QCOMPARE(runs.at(1), (QList<int>{2}));
    QCOMPARE(runs.at(2), (QList<int>{4, 5}));
    QCOMPARE(runs.at(3), (QList<int>{6}));
    QCOMPARE(runs.at(4), (QList<int>{3}));
    // A camera's file names count as one subject.
    QCOMPARE(Unsorted::groupKey(m_osprey, QStringLiteral("DSC_0001")),
             Unsorted::groupKey(m_osprey, QStringLiteral("DSC_0002")));
    QVERIFY(Unsorted::groupKey(m_osprey, QStringLiteral("Osprey - Hervey Bay 4-6-2026 (1)"))
            != Unsorted::groupKey(m_osprey, QStringLiteral("Brown Honeyeater - Hervey Bay 4-6-2026 (1)")));
}

void TestUnsortedFolders::markingKeepsOnePerRunAndWhatYouDecided()
{
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(10, 0, 0), 5);   // one run
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(11, 0, 0), 3);   // another
    QVERIFY(scan().ok());
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 8);
    // A best shot in the second run: kept, and its representative.
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("INSERT INTO best_shot (capture_id) SELECT id FROM capture WHERE base_name LIKE '%(8)'"));

    const Unsorted::ThinResult r = Unsorted::mark(m_db->connectionName(), m_root + QStringLiteral("/To Sort"));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.photos, 8);
    QCOMPARE(r.runs, 2);
    QCOMPARE(r.kept, 2);
    QCOMPARE(r.removed, 6);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 2);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture WHERE base_name LIKE '%(8)'")), 1);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture WHERE base_name LIKE '%(3)'")), 1);   // the middle of 5
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM unsorted_photo")), 6);
    // Every file is still there.
    QCOMPARE(QDir(m_osprey).entryList(QDir::Files).size(), 8);
    const QList<Unsorted::FolderStats> stats = Unsorted::stats(m_db->connectionName());
    QCOMPARE(stats.size(), 1);
    QCOMPARE(stats.first().catalogued, 2);
    QCOMPARE(stats.first().remembered, 6);
}

void TestUnsortedFolders::rescansSkipRememberedShotsAndAddNewRuns()
{
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(10, 0, 0), 5);
    QVERIFY(scan().ok());
    QVERIFY(Unsorted::mark(m_db->connectionName(), m_root + QStringLiteral("/To Sort")).ok);
    ScanSummary again = scan();
    QVERIFY(again.ok());
    QCOMPARE(again.filesHashed, 0);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 1);

    // Two shots added to the run (within 10 s), and a new run of four.
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(10, 0, 9), 2);
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(12, 0, 0), 4);
    again = scan();
    QVERIFY2(again.ok(), qPrintable(again.error));
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 2);   // + the new run's representative
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM unsorted_photo")), 4 + 2 + 3);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM unsorted_photo WHERE capture_id IS NULL")), 0);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(DISTINCT capture_id) FROM unsorted_photo")), 2);
}

void TestUnsortedFolders::cullingRemovesAndPromotes()
{
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(10, 0, 0), 5);
    QVERIFY(scan().ok());
    QVERIFY(Unsorted::mark(m_db->connectionName(), m_root + QStringLiteral("/To Sort")).ok);
    // Culled: one remembered shot, and the representative's file.
    QVERIFY(QFile::remove(m_osprey + QStringLiteral("/Osprey - Hervey Bay, 2026-06-04 (1).jpg")));
    QVERIFY(QFile::remove(m_osprey + QStringLiteral("/Osprey - Hervey Bay, 2026-06-04 (3).jpg")));
    const ScanSummary s = scan();
    QVERIFY2(s.ok(), qPrintable(s.error));
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM unsorted_photo")), 2);   // 2 and 5 remembered, 4 promoted
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture c JOIN rendition r ON r.capture_id = c.id "
                                  "WHERE r.path LIKE '%(4).jpg'")),
             1);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(DISTINCT u.capture_id) FROM unsorted_photo u JOIN rendition r "
                                  "ON r.capture_id = u.capture_id WHERE r.path LIKE '%(4).jpg'")),
             1);
}

void TestUnsortedFolders::unmarkingCataloguesEverythingAgain()
{
    shoot(QStringLiteral("Osprey - Hervey Bay, 2026-06-04"), QTime(10, 0, 0), 5);
    QVERIFY(scan().ok());
    QVERIFY(Unsorted::mark(m_db->connectionName(), m_root + QStringLiteral("/To Sort")).ok);
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 1);
    QVERIFY(Unsorted::unmark(m_db->connectionName(), m_root + QStringLiteral("/To Sort")));
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM unsorted_photo")), 0);
    QVERIFY(scan().ok());
    QCOMPARE(count(QStringLiteral("SELECT COUNT(*) FROM capture")), 5);
    QVERIFY(Unsorted::folders(m_db->connectionName()).isEmpty());
}

QTEST_GUILESS_MAIN(TestUnsortedFolders)
#include "tst_unsortedfolders.moc"
