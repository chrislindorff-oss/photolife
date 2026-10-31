#include <QtTest>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "geo/GeotagWriter.h"
#include "inat/ExifWriter.h"
#include "scan/Exif.h"

using namespace pl;
using namespace pl::inat;

namespace {

void putU16(QByteArray &b, quint16 v) { b.append(char(v & 0xff)).append(char(v >> 8)); }
void putU32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xff));
}
void putEntry(QByteArray &b, quint16 tag, quint16 type, quint32 count, quint32 value)
{
    putU16(b, tag);
    putU16(b, type);
    putU32(b, count);
    putU32(b, value);
}

// A tiny but complete little-endian TIFF -- as a TIFF-based RAW is laid out:
// one grey pixel, Make/Model, and an EXIF IFD with DateTimeOriginal.
QByteArray tinyTiffRaw()
{
    const QByteArray make("Nikon\0", 6), model("D500\0", 5);
    const QByteArray date("2026:06:03 16:03:53\0", 20);
    const quint32 ifd0 = 8, entries = 11;
    const quint32 afterIfd0 = ifd0 + 2 + entries * 12 + 4;
    const quint32 makeAt = afterIfd0, modelAt = makeAt + 6, exifAt = modelAt + 6;   // even offsets
    const quint32 dateAt = exifAt + 2 + 12 + 4, pixelAt = dateAt + 20;
    QByteArray b("II");
    putU16(b, 42);
    putU32(b, ifd0);
    putU16(b, entries);
    putEntry(b, 0x0100, 3, 1, 1);          // ImageWidth
    putEntry(b, 0x0101, 3, 1, 1);          // ImageLength
    putEntry(b, 0x0102, 3, 1, 8);          // BitsPerSample
    putEntry(b, 0x0103, 3, 1, 1);          // Compression: none
    putEntry(b, 0x0106, 3, 1, 1);          // Photometric: black is zero
    putEntry(b, 0x010F, 2, 6, makeAt);     // Make
    putEntry(b, 0x0110, 2, 5, modelAt);    // Model
    putEntry(b, 0x0111, 4, 1, pixelAt);    // StripOffsets
    putEntry(b, 0x0116, 3, 1, 1);          // RowsPerStrip
    putEntry(b, 0x0117, 4, 1, 1);          // StripByteCounts
    putEntry(b, 0x8769, 4, 1, exifAt);     // Exif IFD
    putU32(b, 0);
    b.append(make);
    b.append(model).append('\0');
    putU16(b, 1);
    putEntry(b, 0x9003, 2, 20, dateAt);    // DateTimeOriginal
    putU32(b, 0);
    b.append(date);
    b.append(char(0x80));                  // the pixel
    return b;
}

} // namespace

// Writes metadata via exiv2, then round-trips it back out through the
// existing (unmodified) EXIF reader to confirm it actually lands correctly.
class TestExifWriter : public QObject
{
    Q_OBJECT

private slots:
    void writesAndRoundTripsMetadata();
    void safeWriteReplacesOriginalWithVerifiedResult();
    void safeWriteLeavesNoTempFileBehindOnFailure();
    void sidecarGpsIsWrittenAndReadBack();
    void locationGoesIntoJpegAndRawSidecar();
    void rawWriteIsVerifiedAndKeepsTheOriginalOnFailure();
};

void TestExifWriter::writesAndRoundTripsMetadata()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("photo.jpg"));
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::green);
    QVERIFY(img.save(path, "JPG"));

    ExifFields fields;
    fields.author = QStringLiteral("Test Author");
    fields.dateTimeOriginal = QStringLiteral("2025-01-10");
    fields.latitude = -37.8136;
    fields.longitude = 144.9631;
    fields.taxonName = QStringLiteral("Diuris sp900");

    QString error;
    QVERIFY2(writeExif(path, fields, &error), qPrintable(error));

    const scan::ExifData read = scan::readJpegExif(path);
    QVERIFY(read.hasDate());
    QCOMPARE(read.dateTimeOriginal.date(), QDate(2025, 1, 10));
    QVERIFY(read.hasGps());
    QVERIFY(qAbs(*read.latitude - (-37.8136)) < 0.0001);
    QVERIFY(qAbs(*read.longitude - 144.9631) < 0.0001);
}

void TestExifWriter::safeWriteReplacesOriginalWithVerifiedResult()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("photo.jpg"));
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::green);
    QVERIFY(img.save(path, "JPG"));

    ExifFields fields;
    fields.latitude = -37.8136;
    fields.longitude = 144.9631;

    QString error;
    QVERIFY2(writeExifSafely(path, fields, &error), qPrintable(error));

    // No leftover working file, and the original path now has the new GPS.
    QVERIFY(!QFile::exists(path + QStringLiteral(".photolife-tmp")));
    const scan::ExifData read = scan::readJpegExif(path);
    QVERIFY(read.hasGps());
    QVERIFY(qAbs(*read.latitude - (-37.8136)) < 0.0001);
    QVERIFY(qAbs(*read.longitude - 144.9631) < 0.0001);

    // The image itself wasn't damaged by the copy/replace.
    QImage after(path);
    QCOMPARE(after.size(), img.size());
}

void TestExifWriter::safeWriteLeavesNoTempFileBehindOnFailure()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("does-not-exist.jpg"));

    ExifFields fields;
    fields.latitude = -37.8136;
    fields.longitude = 144.9631;

    QString error;
    QVERIFY(!writeExifSafely(path, fields, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(path));
    QVERIFY(!QFile::exists(path + QStringLiteral(".photolife-tmp")));
}

void TestExifWriter::sidecarGpsIsWrittenAndReadBack()
{
    QTemporaryDir dir;
    const QString sidecar = dir.filePath(QStringLiteral("DSC_0001.xmp"));
    // A new sidecar...
    QVERIFY(writeSidecarGps(sidecar, -25.954235, 153.104513));
    scan::ExifData gps = scan::readXmpGps(sidecar);
    QVERIFY(gps.hasGps());
    QVERIFY(qAbs(*gps.latitude - -25.954235) < 1e-6);
    QVERIFY(qAbs(*gps.longitude - 153.104513) < 1e-6);
    // ...and an existing one keeps what else it holds.
    QVERIFY(writeSidecarRating(sidecar, 5));
    QVERIFY(writeSidecarGps(sidecar, 51.5, -0.1278));
    gps = scan::readXmpGps(sidecar);
    QVERIFY(qAbs(*gps.latitude - 51.5) < 1e-6);
    QVERIFY(qAbs(*gps.longitude - -0.1278) < 1e-6);
    QCOMPARE(scan::readXmpRating(sidecar).value_or(0), 5);

    QCOMPARE(xmpCoordinate(-25.5, true), QStringLiteral("25,30.00000S"));
    QVERIFY(qAbs(scan::parseXmpCoordinate(QStringLiteral("153,6.27078E")).value_or(0) - 153.104513) < 1e-6);
    QVERIFY(qAbs(scan::parseXmpCoordinate(QStringLiteral("37,48,30W")).value_or(0) - -(37 + 48 / 60.0 + 30 / 3600.0)) < 1e-9);
    QVERIFY(!scan::parseXmpCoordinate(QStringLiteral("nonsense")));
}

static QByteArray contents(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static void writeFile(const QString &path, const QByteArray &bytes, const QDateTime &modified)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    f.close();
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(modified, QFileDevice::FileModificationTime));
}

void TestExifWriter::locationGoesIntoJpegAndRawSidecar()
{
    QTemporaryDir dir;
    const QString jpeg = dir.filePath(QStringLiteral("DSC_0001.JPG"));
    const QString nef = dir.filePath(QStringLiteral("DSC_0001.NEF"));
    const QDateTime old = QDateTime::currentDateTime().addDays(-3);
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::green);
    QVERIFY(img.save(jpeg, "JPG"));
    writeFile(jpeg, contents(jpeg), old);
    writeFile(nef, tinyTiffRaw(), old);
    const QByteArray rawBefore = contents(nef);

    const geo::GeotagWriter::Result r =
        geo::GeotagWriter::writeShot({nef, jpeg}, QStringLiteral("DSC_0001"), -25.954, 153.105, false);
    QVERIFY2(r.ok(), qPrintable(r.failures.join(QLatin1Char('\n'))));
    // The JPEG has it, and keeps its date.
    const scan::ExifData exif = scan::readJpegExif(jpeg);
    QVERIFY(exif.hasGps());
    QVERIFY(qAbs(*exif.latitude - -25.954) < 1e-5);
    QCOMPARE(QFileInfo(jpeg).lastModified().toSecsSinceEpoch(), old.toSecsSinceEpoch());
    // The RAW is untouched; its new sidecar has it.
    QCOMPARE(r.createdSidecar, dir.filePath(QStringLiteral("DSC_0001.xmp")));
    QCOMPARE(contents(nef), rawBefore);
    QVERIFY(qAbs(*scan::readXmpGps(r.createdSidecar).longitude - 153.105) < 1e-6);

    // darktable's per-file sidecar is used rather than making another.
    QVERIFY(QFile::remove(r.createdSidecar));
    QVERIFY(writeSidecarRating(nef + QStringLiteral(".xmp"), 3));
    const geo::GeotagWriter::Result again =
        geo::GeotagWriter::writeShot({nef}, QStringLiteral("DSC_0001"), 1.0, 2.0, false);
    QVERIFY(again.ok());
    QVERIFY(again.createdSidecar.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("DSC_0001.xmp"))));
    QVERIFY(qAbs(*scan::readXmpGps(nef + QStringLiteral(".xmp")).latitude - 1.0) < 1e-6);
}

void TestExifWriter::rawWriteIsVerifiedAndKeepsTheOriginalOnFailure()
{
    QTemporaryDir dir;
    const QString nef = dir.filePath(QStringLiteral("DSC_0002.NEF"));
    const QDateTime old = QDateTime::currentDateTime().addDays(-3);
    writeFile(nef, tinyTiffRaw(), old);
    QVERIFY(scan::readTiffExif(nef).hasDate());
    QString error;
    QVERIFY2(geo::GeotagWriter::writeIntoRaw(nef, -37.8136, 144.9631, &error), qPrintable(error));
    const scan::ExifData after = scan::readTiffExif(nef);
    QVERIFY(after.hasGps());
    QVERIFY(qAbs(*after.latitude - -37.8136) < 1e-5);
    QCOMPARE(after.dateTimeOriginal, QDateTime(QDate(2026, 6, 3), QTime(16, 3, 53)));
    QCOMPARE(after.cameraModel, QStringLiteral("D500"));
    QCOMPARE(QFileInfo(nef).lastModified().toSecsSinceEpoch(), old.toSecsSinceEpoch());
    QVERIFY(!QFileInfo::exists(nef + QStringLiteral(".photolife-tmp")));

    // Not a readable RAW: left exactly as it was, nothing left behind.
    const QString broken = dir.filePath(QStringLiteral("DSC_0003.NEF"));
    const QByteArray junk(4096, 'x');
    writeFile(broken, junk, old);
    QVERIFY(!geo::GeotagWriter::writeIntoRaw(broken, 1, 2, &error));
    QCOMPARE(contents(broken), junk);
    QVERIFY(!QFileInfo::exists(broken + QStringLiteral(".photolife-tmp")));
    // A kind exiv2 can't write: refused.
    QVERIFY(!geo::GeotagWriter::canWriteIntoRaw(QStringLiteral("IMG_0001.CR3")));
    QVERIFY(!geo::GeotagWriter::writeIntoRaw(dir.filePath(QStringLiteral("IMG_0001.CR3")), 1, 2));
}

QTEST_GUILESS_MAIN(TestExifWriter)
#include "tst_exifwriter.moc"
