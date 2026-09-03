#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "scan/LibraryWatcher.h"

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

class TestLibraryWatcher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void watchesRootAndCataloguedFolders();
    void newFileInWatchedFolderRaisesOneDebouncedSignal();

private:
    QTemporaryDir m_tmp;
    QString m_root;
    std::unique_ptr<Database> m_db;

    void populate();
};

void TestLibraryWatcher::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Library"));
    writeFile(m_root + QStringLiteral("/Genus/Genus species/a - Loc 1-1-2020.jpg"),
              QByteArrayLiteral("one"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    populate();
}

void TestLibraryWatcher::cleanup()
{
    m_db.reset();
}

void TestLibraryWatcher::populate()
{
    FileScanner scanner;
    CatalogueWriter writer(m_db->connectionName());
    const ScanSummary s = writer.sync(scanner.scan({m_root}), {m_root});
    QVERIFY2(s.ok(), qPrintable(s.error));
}

void TestLibraryWatcher::watchesRootAndCataloguedFolders()
{
    LibraryWatcher watcher(*m_db);
    watcher.setRoots({m_root});
    // root + Genus + "Genus species"
    QCOMPARE(watcher.watchedDirectoryCount(), 3);
}

void TestLibraryWatcher::newFileInWatchedFolderRaisesOneDebouncedSignal()
{
    LibraryWatcher watcher(*m_db);
    watcher.setDebounceInterval(150);
    watcher.setRoots({m_root});

    QSignalSpy spy(&watcher, &LibraryWatcher::changeDetected);

    writeFile(m_root + QStringLiteral("/Genus/Genus species/b - Loc 2-1-2020.jpg"),
              QByteArrayLiteral("two"));
    writeFile(m_root + QStringLiteral("/Genus/Genus species/c - Loc 3-1-2020.jpg"),
              QByteArrayLiteral("three"));

    QVERIFY(spy.wait(5000));
    QTest::qWait(400);   // let any further debounced fire land
    QCOMPARE(spy.count(), 1);
}

QTEST_GUILESS_MAIN(TestLibraryWatcher)
#include "tst_librarywatcher.moc"
