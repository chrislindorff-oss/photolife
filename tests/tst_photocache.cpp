#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "net/PhotoCache.h"

using namespace pl::net;

class TestPhotoCache : public QObject
{
    Q_OBJECT

private slots:
    void emptyUrlIsNull();
    void returnsAnImageAlreadyOnDisk();
    void unreachableUrlStaysNullAndIsNotRetriedForever();
    void retryAsksAgainForAFailedUrl();
    void diskUsageCountsOnlyUrlsPresentOnDisk();
    void largerVariantsAskForTheBiggestFirst();

private:
    static QString diskPathFor(const QString &cacheDir, const QString &url)
    {
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
        return QDir(cacheDir).filePath(hash.left(2) + QLatin1Char('/') + hash
                                       + QStringLiteral(".jpg"));
    }
};

void TestPhotoCache::emptyUrlIsNull()
{
    QTemporaryDir dir;
    PhotoCache cache(dir.path(), QByteArrayLiteral("test/1"));
    QVERIFY(cache.photo(QString()).isNull());
}

void TestPhotoCache::returnsAnImageAlreadyOnDisk()
{
    QTemporaryDir dir;
    const QString url = QStringLiteral("https://inat.example/photos/1/medium.jpg");

    const QString path = diskPathFor(dir.path(), url);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::green);
    QVERIFY(img.save(path, "JPG"));

    PhotoCache cache(dir.path(), QByteArrayLiteral("test/1"));
    const QPixmap pm = cache.photo(url);
    QVERIFY(!pm.isNull());
    QCOMPARE(pm.size(), QSize(20, 20));
}

void TestPhotoCache::unreachableUrlStaysNullAndIsNotRetriedForever()
{
    QTemporaryDir dir;
    PhotoCache cache(dir.path(), QByteArrayLiteral("test/1"));
    cache.setRetryBaseMs(20);

    // A syntactically valid but unroutable URL: each try fails fast. It's
    // tried a few times (a dropped connection can be passing), then marked
    // failed -- one ready() for the lot -- so a second ask schedules nothing.
    const QString url = QStringLiteral("http://127.0.0.1:9/nope.jpg");
    QSignalSpy spy(&cache, &PhotoCache::ready);
    QVERIFY(cache.photo(url).isNull());
    QVERIFY(!cache.hasFailed(url));   // queued, not failed yet
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), url);
    QVERIFY(cache.hasFailed(url));
    QVERIFY(cache.photo(url).isNull());   // still null, no crash, no infinite retry
    QVERIFY(!spy.wait(300));              // ...and nothing more scheduled
}

void TestPhotoCache::retryAsksAgainForAFailedUrl()
{
    QTemporaryDir dir;
    PhotoCache cache(dir.path(), QByteArrayLiteral("test/1"));
    cache.setRetryBaseMs(20);
    const QString url = QStringLiteral("http://127.0.0.1:9/nope.jpg");
    QSignalSpy spy(&cache, &PhotoCache::ready);
    cache.photo(url);
    QVERIFY(spy.wait(5000));
    QVERIFY(cache.hasFailed(url));

    cache.retry(url);
    QVERIFY(!cache.hasFailed(url));   // asked for again
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 2);
    QVERIFY(cache.hasFailed(url));
}

void TestPhotoCache::diskUsageCountsOnlyUrlsPresentOnDisk()
{
    QTemporaryDir dir;
    const QString cachedUrl = QStringLiteral("https://inat.example/photos/1/medium.jpg");
    const QString missingUrl = QStringLiteral("https://inat.example/photos/2/medium.jpg");

    const QString path = diskPathFor(dir.path(), cachedUrl);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::green);
    QVERIFY(img.save(path, "JPG"));

    PhotoCache cache(dir.path(), QByteArrayLiteral("test/1"));
    const PhotoCache::DiskUsage usage = cache.diskUsage({cachedUrl, missingUrl, QString()});
    QCOMPARE(usage.count, 1);
    QCOMPARE(usage.bytes, QFileInfo(path).size());
}

void TestPhotoCache::largerVariantsAskForTheBiggestFirst()
{
    using pl::net::PhotoCache;
    QCOMPARE(PhotoCache::largerVariants(
                 QStringLiteral("https://inaturalist-open-data.s3.amazonaws.com/photos/356885346/medium.jpg")),
             (QStringList{QStringLiteral("https://inaturalist-open-data.s3.amazonaws.com/photos/356885346/original.jpg"),
                          QStringLiteral("https://inaturalist-open-data.s3.amazonaws.com/photos/356885346/large.jpg"),
                          QStringLiteral("https://inaturalist-open-data.s3.amazonaws.com/photos/356885346/medium.jpg")}));
    // Other extensions and a cache-busting query are kept.
    QCOMPARE(PhotoCache::largerVariants(QStringLiteral("https://static.inaturalist.org/photos/9/small.jpeg?1600"))
                 .first(),
             QStringLiteral("https://static.inaturalist.org/photos/9/original.jpeg?1600"));
    // Already the largest, or not an iNaturalist size: just itself.
    QCOMPARE(PhotoCache::largerVariants(QStringLiteral("https://x.example/photos/1/original.jpg")),
             (QStringList{QStringLiteral("https://x.example/photos/1/original.jpg"),
                          QStringLiteral("https://x.example/photos/1/large.jpg")}));
    QCOMPARE(PhotoCache::largerVariants(QStringLiteral("https://x.example/a.jpg")),
             QStringList{QStringLiteral("https://x.example/a.jpg")});
}

QTEST_MAIN(TestPhotoCache)
#include "tst_photocache.moc"
