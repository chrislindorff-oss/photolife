#include <QtTest>

#include <QBuffer>

#include "scan/Exif.h"

using namespace pl::scan;

namespace {

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

// A little-endian TIFF blob (the bytes that follow "Exif\0\0"): IFD0 carries
// Make + an Exif-IFD pointer; the Exif IFD carries DateTimeOriginal.
QByteArray sampleTiff()
{
    const QByteArray make = QByteArrayLiteral("Nikon\0");             // 6 bytes
    const QByteArray dto = QByteArrayLiteral("2020:09:28 07:31:00\0"); // 20 bytes

    const quint32 kMakeAt = 38;
    const quint32 kExifIfdAt = 44;
    const quint32 kDtoAt = 62;

    QByteArray b;
    b.append("II");
    putU16(b, 0x002A);
    putU32(b, 8);                       // IFD0 at offset 8

    putU16(b, 2);                       // IFD0: 2 entries
    putEntry(b, 0x010F, 2, make.size(), kMakeAt);
    putEntry(b, 0x8769, 4, 1, kExifIfdAt);
    putU32(b, 0);                       // no IFD1
    Q_ASSERT(b.size() == int(kMakeAt));
    b.append(make);

    Q_ASSERT(b.size() == int(kExifIfdAt));
    putU16(b, 1);                       // Exif IFD: 1 entry
    putEntry(b, 0x9003, 2, dto.size(), kDtoAt);
    putU32(b, 0);
    Q_ASSERT(b.size() == int(kDtoAt));
    b.append(dto);

    return b;
}

QByteArray wrapAsJpeg(const QByteArray &tiff)
{
    QByteArray payload = QByteArrayLiteral("Exif\0\0") + tiff;
    QByteArray jpeg;
    jpeg.append('\xFF').append('\xD8');                   // SOI
    jpeg.append('\xFF').append('\xE1');                   // APP1
    putU16(jpeg, quint16(payload.size() + 2));            // big-endian segment length
    // putU16 is little-endian; JPEG segment length is big-endian, so fix it:
    jpeg[jpeg.size() - 2] = char((payload.size() + 2) >> 8);
    jpeg[jpeg.size() - 1] = char((payload.size() + 2) & 0xFF);
    jpeg.append(payload);
    jpeg.append('\xFF').append('\xD9');                   // EOI
    return jpeg;
}

} // namespace

class TestExif : public QObject
{
    Q_OBJECT

private slots:
    void parsesDateAndMakeFromSegment();
    void parsesFromJpegFraming();
    void nonJpegYieldsEmpty();
    void jpegWithoutExifYieldsEmpty();
    void truncatedSegmentDoesNotCrash();
};

void TestExif::parsesDateAndMakeFromSegment()
{
    const ExifData e = parseExifSegment(sampleTiff());
    QVERIFY(e.hasDate());
    QCOMPARE(e.dateTimeOriginal.date(), QDate(2020, 9, 28));
    QCOMPARE(e.dateTimeOriginal.time(), QTime(7, 31, 0));
    QCOMPARE(e.cameraMake, QStringLiteral("Nikon"));
}

void TestExif::parsesFromJpegFraming()
{
    QByteArray jpeg = wrapAsJpeg(sampleTiff());
    QBuffer buf(&jpeg);
    QVERIFY(buf.open(QIODevice::ReadOnly));
    const ExifData e = readJpegExif(buf);
    QCOMPARE(e.dateTimeOriginal.date(), QDate(2020, 9, 28));
}

void TestExif::nonJpegYieldsEmpty()
{
    QByteArray png = QByteArrayLiteral("\x89PNG\r\n\x1a\n and then some");
    QBuffer buf(&png);
    QVERIFY(buf.open(QIODevice::ReadOnly));
    QVERIFY(!readJpegExif(buf).hasDate());
}

void TestExif::jpegWithoutExifYieldsEmpty()
{
    QByteArray jpeg;
    jpeg.append('\xFF').append('\xD8').append('\xFF').append('\xD9');
    QBuffer buf(&jpeg);
    QVERIFY(buf.open(QIODevice::ReadOnly));
    QVERIFY(!readJpegExif(buf).hasDate());
}

void TestExif::truncatedSegmentDoesNotCrash()
{
    QByteArray tiff = sampleTiff();
    for (int cut = tiff.size(); cut > 0; cut -= 3)
        (void)parseExifSegment(tiff.left(cut));  // must not crash or hang
    QVERIFY(true);
}

QTEST_APPLESS_MAIN(TestExif)
#include "tst_exif.moc"
