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

// An ASCII entry whose value fits inline (<=4 bytes), so the value field holds
// the raw text itself rather than a pointer.
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

// Like sampleTiff(), but IFD0 also points at a GPS IFD carrying
// GPSLatitude(Ref)/GPSLongitude(Ref). latRef/lonRef select the hemisphere
// ("S"/"N" and "W"/"E") to exercise the sign logic in both directions.
QByteArray sampleTiffWithGps(const QByteArray &latRef, const QByteArray &lonRef)
{
    const QByteArray make = QByteArrayLiteral("Nikon\0");
    const QByteArray dto = QByteArrayLiteral("2020:09:28 07:31:00\0");

    const quint32 kMakeAt = 50;
    const quint32 kExifIfdAt = 56;
    const quint32 kDtoAt = 74;
    const quint32 kGpsIfdAt = 94;
    const quint32 kGpsLatAt = 148;
    const quint32 kGpsLonAt = 172;

    QByteArray b;
    b.append("II");
    putU16(b, 0x002A);
    putU32(b, 8);                       // IFD0 at offset 8

    putU16(b, 3);                       // IFD0: 3 entries
    putEntry(b, 0x010F, 2, make.size(), kMakeAt);
    putEntry(b, 0x8769, 4, 1, kExifIfdAt);
    putEntry(b, 0x8825, 4, 1, kGpsIfdAt);
    putU32(b, 0);                       // no IFD1
    Q_ASSERT(b.size() == int(kMakeAt));
    b.append(make);

    Q_ASSERT(b.size() == int(kExifIfdAt));
    putU16(b, 1);                       // Exif IFD: 1 entry
    putEntry(b, 0x9003, 2, dto.size(), kDtoAt);
    putU32(b, 0);
    Q_ASSERT(b.size() == int(kDtoAt));
    b.append(dto);

    Q_ASSERT(b.size() == int(kGpsIfdAt));
    putU16(b, 4);                                    // GPS IFD: 4 entries
    putEntryInlineAscii(b, 0x0001, latRef);          // GPSLatitudeRef
    putEntry(b, 0x0002, 5, 3, kGpsLatAt);            // GPSLatitude
    putEntryInlineAscii(b, 0x0003, lonRef);          // GPSLongitudeRef
    putEntry(b, 0x0004, 5, 3, kGpsLonAt);            // GPSLongitude
    putU32(b, 0);
    Q_ASSERT(b.size() == int(kGpsLatAt));
    putRational(b, 10, 1);   // 10 deg
    putRational(b, 30, 1);   // 30 min
    putRational(b, 0, 1);    // 0 sec  -> 10.5 degrees
    Q_ASSERT(b.size() == int(kGpsLonAt));
    putRational(b, 20, 1);   // 20 deg
    putRational(b, 15, 1);   // 15 min
    putRational(b, 0, 1);    // 0 sec  -> 20.25 degrees

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
    void parsesGpsSouthWest();
    void parsesGpsNorthEast();
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

    const QByteArray withGps = sampleTiffWithGps("S", "W");
    for (int cut = withGps.size(); cut > 0; cut -= 3)
        (void)parseExifSegment(withGps.left(cut));
    QVERIFY(true);
}

void TestExif::parsesGpsSouthWest()
{
    const ExifData e = parseExifSegment(sampleTiffWithGps("S", "W"));
    QVERIFY(e.hasGps());
    QVERIFY(qAbs(e.latitude.value() - (-10.5)) < 1e-6);
    QVERIFY(qAbs(e.longitude.value() - (-20.25)) < 1e-6);
}

void TestExif::parsesGpsNorthEast()
{
    const ExifData e = parseExifSegment(sampleTiffWithGps("N", "E"));
    QVERIFY(e.hasGps());
    QVERIFY(qAbs(e.latitude.value() - 10.5) < 1e-6);
    QVERIFY(qAbs(e.longitude.value() - 20.25) < 1e-6);
}

QTEST_APPLESS_MAIN(TestExif)
#include "tst_exif.moc"
