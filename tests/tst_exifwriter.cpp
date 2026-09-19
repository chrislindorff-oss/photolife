#include <QtTest>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "inat/ExifWriter.h"
#include "scan/Exif.h"

using namespace pl;
using namespace pl::inat;

// Writes metadata via exiv2, then round-trips it back out through the
// existing (unmodified) EXIF reader to confirm it actually lands correctly.
class TestExifWriter : public QObject
{
    Q_OBJECT

private slots:
    void writesAndRoundTripsMetadata();
    void safeWriteReplacesOriginalWithVerifiedResult();
    void safeWriteLeavesNoTempFileBehindOnFailure();
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

QTEST_GUILESS_MAIN(TestExifWriter)
#include "tst_exifwriter.moc"
