#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include "scan/FileScanner.h"

using namespace pl::scan;

namespace {

void writeFile(const QString &path, const QByteArray &bytes = QByteArrayLiteral("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(bytes);
}

} // namespace

class TestFileScanner : public QObject
{
    Q_OBJECT

private slots:
    void groupsRawAndJpegIntoOneCapture();
    void skipsJunkFilesAndDirs();
    void cancelPredicateStopsTheWalk();
    void missingRootIsSkipped();
};

void TestFileScanner::groupsRawAndJpegIntoOneCapture()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString base = tmp.filePath(
        QStringLiteral("Orchidaceae/Diuris/Diuris pardina"));

    writeFile(base + QStringLiteral("/Diuris pardina - Dadswells 28-9-2020 (1).JPG"));
    writeFile(base + QStringLiteral("/Diuris pardina - Dadswells 28-9-2020 (1).NEF"));
    writeFile(base + QStringLiteral("/Diuris pardina - Dadswells 28-9-2020 (2).jpg"));

    FileScanner scanner;
    const QList<DiscoveredCapture> caps = scanner.scan({tmp.path()});

    QCOMPARE(caps.size(), 2);
    const DiscoveredCapture &first = caps.at(0);
    QCOMPARE(first.baseName,
             QStringLiteral("Diuris pardina - Dadswells 28-9-2020 (1)"));
    QCOMPARE(first.files.size(), 2);
    QVERIFY(!first.files.at(0).isRaw);   // JPEG sorted ahead of RAW
    QVERIFY(first.files.at(1).isRaw);
    QCOMPARE(first.files.at(1).ext, QStringLiteral("nef"));
}

void TestFileScanner::skipsJunkFilesAndDirs()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    writeFile(tmp.filePath(QStringLiteral("Genus species/real - Loc 1-1-2020.jpg")));
    writeFile(tmp.filePath(QStringLiteral("Genus species/Thumbs.db")));
    writeFile(tmp.filePath(QStringLiteral("Genus species/notes.txt")));
    writeFile(tmp.filePath(QStringLiteral("Genus species/preview.xmp")));
    writeFile(tmp.filePath(QStringLiteral("Genus species/.hidden.jpg")));
    writeFile(tmp.filePath(QStringLiteral("NKSC_PARAM/junk.jpg")));
    writeFile(tmp.filePath(QStringLiteral("To Sort and Upload/later.jpg")));

    FileScanner scanner;
    const QList<DiscoveredCapture> caps = scanner.scan({tmp.path()});

    QCOMPARE(caps.size(), 1);
    QCOMPARE(caps.at(0).files.size(), 1);
    QCOMPARE(caps.at(0).baseName, QStringLiteral("real - Loc 1-1-2020"));
}

void TestFileScanner::cancelPredicateStopsTheWalk()
{
    QTemporaryDir tmp;
    writeFile(tmp.filePath(QStringLiteral("a/one - L 1-1-2020.jpg")));
    writeFile(tmp.filePath(QStringLiteral("b/two - L 1-1-2020.jpg")));

    FileScanner scanner;
    scanner.setCancelPredicate([] { return true; });
    const QList<DiscoveredCapture> caps = scanner.scan({tmp.path()});

    QVERIFY(scanner.wasCancelled());
    QVERIFY(caps.isEmpty());
}

void TestFileScanner::missingRootIsSkipped()
{
    FileScanner scanner;
    const QList<DiscoveredCapture> caps =
        scanner.scan({QStringLiteral("/no/such/place/at/all")});
    QVERIFY(caps.isEmpty());
}

QTEST_GUILESS_MAIN(TestFileScanner)
#include "tst_filescanner.moc"
