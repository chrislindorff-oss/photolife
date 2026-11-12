// CardImporter: copying a card's photos into day folders, verified, without
// touching the card, and never twice.
#include "organise/CardImporter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using namespace pl::organise;

namespace {

void writeFile(const QString &path, const QByteArray &data, const QDateTime &modified)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(modified, QFileDevice::FileModificationTime));
}

// Every file under `dir` with its contents' hash and date: to show it didn't change.
QMap<QString, QString> snapshot(const QString &dir)
{
    QMap<QString, QString> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFile f(it.filePath());
        f.open(QIODevice::ReadOnly);
        out.insert(it.filePath(), QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex())
                                      + QString::number(it.fileInfo().lastModified().toSecsSinceEpoch()));
    }
    return out;
}

const QDateTime kDay1(QDate(2026, 11, 12), QTime(9, 30));
const QDateTime kDay2(QDate(2026, 11, 13), QTime(7, 15));

} // namespace

class TestCardImporter : public QObject
{
    Q_OBJECT

private slots:
    void folderNames();
    void copiesIntoDayFoldersAndLeavesTheCardAlone();
    void aSecondImportSkipsWhatCameOff();
    void aNameTakenByAnotherPhotoGetsANewName();
    void theDayTakenComesFromTheExifDate();
    void cancelStopsPartWay();
    void recognisesACard();
};

void TestCardImporter::folderNames()
{
    QCOMPARE(CardImporter::folderName(QDate(2026, 11, 13), QString()), QStringLiteral("2026-11-13"));
    QCOMPARE(CardImporter::folderName(QDate(2026, 11, 13), QStringLiteral("  Anakie  ")),
             QStringLiteral("2026-11-13 Anakie"));
    QCOMPARE(CardImporter::folderName(QDate(2026, 11, 13), QStringLiteral("Lake: east/west")),
             QStringLiteral("2026-11-13 Lake- east-west"));
    QCOMPARE(CardImporter::folderName(QDate(), QString()), QStringLiteral("Undated"));
}

void TestCardImporter::copiesIntoDayFoldersAndLeavesTheCardAlone()
{
    QTemporaryDir card, disk;
    const QDir c(card.path());
    writeFile(c.filePath(QStringLiteral("DCIM/100NIKON/DSC_0001.NEF")), "raw one", kDay1);
    writeFile(c.filePath(QStringLiteral("DCIM/100NIKON/DSC_0001.JPG")), "jpeg one", kDay1);
    writeFile(c.filePath(QStringLiteral("DCIM/100NIKON/DSC_0001.xmp")), "<xmp/>", kDay2);   // goes with its photo's day
    writeFile(c.filePath(QStringLiteral("DCIM/101NIKON/DSC_0002.NEF")), "raw two", kDay2);
    writeFile(c.filePath(QStringLiteral("DCIM/101NIKON/notes.txt")), "not a photo", kDay2);
    writeFile(c.filePath(QStringLiteral("MISC/.hidden.jpg")), "hidden", kDay2);
    const QMap<QString, QString> before = snapshot(card.path());

    const QString dest = QDir(disk.path()).filePath(QStringLiteral("Downloads"));
    const QList<CardImporter::Source> files = CardImporter::survey(card.path(), dest);
    QCOMPARE(files.size(), 4);
    QList<int> seen;
    const CardImporter::Result r = CardImporter::import(files, dest, QStringLiteral("Anakie"), false,
                                                        [&](int done, int total) {
                                                            QCOMPARE(total, 4);
                                                            seen << done;
                                                        });
    QVERIFY2(r.ok(), qPrintable(r.failures.join(QLatin1Char(';'))));
    QCOMPARE(r.copied, 4);
    QCOMPARE(seen, (QList<int>{1, 2, 3, 4}));
    const QDir d1(QDir(dest).filePath(QStringLiteral("2026-11-12 Anakie")));
    const QDir d2(QDir(dest).filePath(QStringLiteral("2026-11-13 Anakie")));
    QCOMPARE(r.folders, (QStringList{d1.path(), d2.path()}));
    for (const QString &n : {QStringLiteral("DSC_0001.NEF"), QStringLiteral("DSC_0001.JPG"), QStringLiteral("DSC_0001.xmp")})
        QVERIFY2(d1.exists(n), qPrintable(n));
    QVERIFY(d2.exists(QStringLiteral("DSC_0002.NEF")));
    // The same bytes, and the original's date.
    QFile copy(d1.filePath(QStringLiteral("DSC_0001.NEF")));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), QByteArray("raw one"));
    QCOMPARE(QFileInfo(copy).lastModified().toSecsSinceEpoch(), kDay1.toSecsSinceEpoch());
    // No working files left behind.
    QVERIFY(d1.entryList({QStringLiteral("*.photolife-part")}, QDir::Files).isEmpty());
    // The card exactly as it was.
    QCOMPARE(snapshot(card.path()), before);
}

void TestCardImporter::aSecondImportSkipsWhatCameOff()
{
    QTemporaryDir card, disk;
    const QDir c(card.path());
    writeFile(c.filePath(QStringLiteral("DCIM/DSC_0001.NEF")), "raw one", kDay1);
    const QString dest = disk.path();
    QCOMPARE(CardImporter::import(CardImporter::survey(card.path(), dest), dest, QString()).copied, 1);
    // The next day: one new shot on the card beside the old one.
    writeFile(c.filePath(QStringLiteral("DCIM/DSC_0002.NEF")), "raw two", kDay2);
    QList<CardImporter::Source> files = CardImporter::survey(card.path(), dest);
    QCOMPARE(files.size(), 2);
    QVERIFY(files.at(0).alreadyImported);
    QVERIFY(!files.at(1).alreadyImported);
    CardImporter::Result r = CardImporter::import(files, dest, QString());
    QCOMPARE(r.copied, 1);
    QCOMPARE(r.skipped, 1);
    // Sorted and moved on since: still not brought back.
    QVERIFY(QFile::remove(QDir(dest).filePath(QStringLiteral("2026-11-12/DSC_0001.NEF"))));
    files = CardImporter::survey(card.path(), dest);
    QVERIFY(files.at(0).alreadyImported);
    QCOMPARE(CardImporter::import(files, dest, QString()).copied, 0);
    // ...unless asked to.
    r = CardImporter::import(files, dest, QString(), true);
    QCOMPARE(r.copied, 1);   // DSC_0001 again
    QCOMPARE(r.skipped, 1);  // DSC_0002 is there, identical
}

void TestCardImporter::aNameTakenByAnotherPhotoGetsANewName()
{
    QTemporaryDir card, disk;
    writeFile(QDir(card.path()).filePath(QStringLiteral("DCIM/DSC_0001.NEF")), "from the card", kDay1);
    const QString folder = QDir(disk.path()).filePath(QStringLiteral("2026-11-12"));
    writeFile(QDir(folder).filePath(QStringLiteral("DSC_0001.NEF")), "a different photo", kDay1);
    const CardImporter::Result r = CardImporter::import(CardImporter::survey(card.path(), disk.path()), disk.path(), QString());
    QCOMPARE(r.copied, 1);
    QFile kept(QDir(folder).filePath(QStringLiteral("DSC_0001.NEF")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("a different photo"));   // never overwritten
    QFile copied(QDir(folder).filePath(QStringLiteral("DSC_0001-2.NEF")));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArray("from the card"));
}

void TestCardImporter::theDayTakenComesFromTheExifDate()
{
    QTemporaryDir card, disk;
    // A JPEG taken on the 1st but copied onto the card (its file date) on the 13th.
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray jpeg;
    {
        // A minimal EXIF segment with DateTimeOriginal, after SOI.
        QByteArray tiff("MM\0\x2a\0\0\0\x08", 8);
        auto u16 = [&](int v) { tiff.append(char(v >> 8)).append(char(v & 0xff)); };
        auto u32 = [&](int v) { u16(v >> 16); u16(v & 0xffff); };
        u16(1);                                   // IFD0: one entry
        u16(0x8769); u16(4); u32(1); u32(26);     // -> Exif IFD at 26
        u32(0);
        u16(1);                                   // Exif IFD: one entry
        u16(0x9003); u16(2); u32(20); u32(44);    // DateTimeOriginal at 44
        u32(0);
        tiff.append("2026:11:01 10:00:00", 20);
        QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG");
        const QByteArray segment = QByteArray("\xff\xe1", 2) + char((app1.size() + 2) >> 8)
                                   + char((app1.size() + 2) & 0xff) + app1;
        jpeg.insert(2, segment);
    }
    writeFile(QDir(card.path()).filePath(QStringLiteral("DCIM/IMG_1.JPG")), jpeg, kDay2);
    const QList<CardImporter::Source> files = CardImporter::survey(card.path(), disk.path());
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().taken, QDate(2026, 11, 1));
}

void TestCardImporter::cancelStopsPartWay()
{
    QTemporaryDir card, disk;
    for (int i = 0; i < 5; ++i)
        writeFile(QDir(card.path()).filePath(QStringLiteral("DCIM/DSC_%1.NEF").arg(i)), "x", kDay1);
    std::atomic<bool> cancel = false;
    const CardImporter::Result r = CardImporter::import(CardImporter::survey(card.path(), disk.path()), disk.path(), QString(),
                                                        false, [&](int done, int) { cancel = done >= 2; }, &cancel);
    QVERIFY(r.cancelled);
    QCOMPARE(r.copied, 2);
    // What came off is known: the next import takes the rest.
    const QList<CardImporter::Source> again = CardImporter::survey(card.path(), disk.path());
    QCOMPARE(std::count_if(again.cbegin(), again.cend(), [](const CardImporter::Source &s) { return s.alreadyImported; }), 2);
}

void TestCardImporter::recognisesACard()
{
    QTemporaryDir card;
    QVERIFY(!CardImporter::looksLikeCard(card.path()));
    QVERIFY(QDir(card.path()).mkdir(QStringLiteral("DCIM")));
    QVERIFY(CardImporter::looksLikeCard(card.path()));
    (void)CardImporter::cardsMounted();   // whatever is plugged in; mustn't fail
}

QTEST_MAIN(TestCardImporter)
#include "tst_cardimporter.moc"
