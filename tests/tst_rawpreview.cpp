#include <QtTest>

#include <QFileInfo>
#include <QImage>

#include "raw/RawPreview.h"
#include "thumb/ThumbnailCache.h"

using namespace pl;

class TestRawPreview : public QObject
{
    Q_OBJECT

private slots:
    void missingFileYieldsNullImage();
    void nonRawFileYieldsNullImage();
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
}

QTEST_GUILESS_MAIN(TestRawPreview)
#include "tst_rawpreview.moc"
