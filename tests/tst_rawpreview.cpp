#include <QtTest>

#include <QFileInfo>
#include <QImage>

#include "raw/RawPreview.h"
#include "thumb/ThumbnailCache.h"

#include <cmath>

using namespace pl;

class TestRawPreview : public QObject
{
    Q_OBJECT

private slots:
    void missingFileYieldsNullImage();
    void nonRawFileYieldsNullImage();
    void missingFileYieldsEmptyEmbeddedJpeg();
    void nonRawFileYieldsEmptyEmbeddedJpeg();
    void missingFileYieldsNoGps();
    void nonRawFileYieldsNoGps();
    void dmsToDecimalDegreesConvertsCorrectly();
    void installRawLoaderIsSafe();
    void realRawFileIfProvided();
};

void TestRawPreview::missingFileYieldsNullImage()
{
    QVERIFY(raw::extractPreview(QStringLiteral("/no/such/file.nef"), 256).isNull());
}

void TestRawPreview::nonRawFileYieldsNullImage()
{
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write("not a raw file");
    f.flush();
    QVERIFY(raw::extractPreview(f.fileName(), 256).isNull());
}

void TestRawPreview::missingFileYieldsEmptyEmbeddedJpeg()
{
    QVERIFY(raw::extractEmbeddedJpegBytes(QStringLiteral("/no/such/file.nef")).isEmpty());
}

void TestRawPreview::nonRawFileYieldsEmptyEmbeddedJpeg()
{
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write("not a raw file");
    f.flush();
    QVERIFY(raw::extractEmbeddedJpegBytes(f.fileName()).isEmpty());
}

void TestRawPreview::missingFileYieldsNoGps()
{
    const raw::RawGps gps = raw::extractGps(QStringLiteral("/no/such/file.nef"));
    QVERIFY(!gps.latitude.has_value());
    QVERIFY(!gps.longitude.has_value());
}

void TestRawPreview::nonRawFileYieldsNoGps()
{
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write("not a raw file");
    f.flush();
    const raw::RawGps gps = raw::extractGps(f.fileName());
    QVERIFY(!gps.latitude.has_value());
    QVERIFY(!gps.longitude.has_value());
}

void TestRawPreview::dmsToDecimalDegreesConvertsCorrectly()
{
    // Southern hemisphere: ref matches negativeRef, so the result is negated.
    const double lat = raw::dmsToDecimalDegrees(37, 52, 13.843047, 'S', 'S');
    QVERIFY(lat < 0.0);
    QVERIFY2(std::abs(lat - (-37.870512)) < 1e-5, qPrintable(QString::number(lat, 'f', 8)));

    // Northern hemisphere / eastern longitude: ref does not match negativeRef.
    const double lon = raw::dmsToDecimalDegrees(144, 14, 47.151675, 'E', 'W');
    QVERIFY(lon > 0.0);
    QVERIFY2(std::abs(lon - 144.246431) < 1e-5, qPrintable(QString::number(lon, 'f', 8)));

    // Case-insensitive ref comparison.
    QCOMPARE(raw::dmsToDecimalDegrees(1, 0, 0, 's', 'S') < 0.0, true);
}

void TestRawPreview::installRawLoaderIsSafe()
{
    QTemporaryDir dir;
    thumb::ThumbnailCache cache(dir.path());
    raw::installRawLoader(cache);   // must not crash whether or not LibRaw is present
    QVERIFY(true);
}

void TestRawPreview::realRawFileIfProvided()
{
    const QByteArray path = qgetenv("PHOTOLIFE_TEST_RAW_FILE");
    if (path.isEmpty())
        QSKIP("set PHOTOLIFE_TEST_RAW_FILE to a camera RAW file to exercise the real decode");
    if (!raw::isAvailable())
        QSKIP("this build has no LibRaw");
    QVERIFY2(QFileInfo::exists(QString::fromLocal8Bit(path)), path.constData());

    const QImage image = raw::extractPreview(QString::fromLocal8Bit(path), 512);
    QVERIFY(!image.isNull());
    QVERIFY(qMax(image.width(), image.height()) >= 256);

    const QByteArray jpegBytes = raw::extractEmbeddedJpegBytes(QString::fromLocal8Bit(path));
    QVERIFY(!jpegBytes.isEmpty());
    QVERIFY(quint8(jpegBytes.at(0)) == 0xFF && quint8(jpegBytes.at(1)) == 0xD8);   // JPEG SOI

    // GPS is optional on the test file, so only sanity-check the range when present.
    const raw::RawGps gps = raw::extractGps(QString::fromLocal8Bit(path));
    if (gps.latitude)
        QVERIFY(*gps.latitude >= -90.0 && *gps.latitude <= 90.0);
    if (gps.longitude)
        QVERIFY(*gps.longitude >= -180.0 && *gps.longitude <= 180.0);
}

QTEST_GUILESS_MAIN(TestRawPreview)
#include "tst_rawpreview.moc"
