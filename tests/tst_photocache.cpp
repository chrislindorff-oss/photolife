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
    void diskUsageCountsOnlyUrlsPresentOnDisk();

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

    // A syntactically valid but unroutable URL: the request fails fast and the
    // url is marked failed, so a second ask does not schedule anything.
    const QString url = QStringLiteral("http://127.0.0.1:9/nope.jpg");
    QSignalSpy spy(&cache, &PhotoCache::ready);
    QVERIFY(cache.photo(url).isNull());
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toString(), url);
    QVERIFY(cache.photo(url).isNull());   // still null, no crash, no infinite retry
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

QTEST_MAIN(TestPhotoCache)
#include "tst_photocache.moc"
