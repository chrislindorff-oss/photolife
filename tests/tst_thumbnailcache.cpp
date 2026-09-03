#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "thumb/ThumbnailCache.h"

using namespace pl::thumb;

namespace {

QString paintSource(const QTemporaryDir &dir, const QString &name, QSize size, QColor colour)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(colour);
    QPainter p(&img);
    p.fillRect(QRect(0, 0, size.width() / 2, size.height()), Qt::black);
    p.end();

    const QString path = dir.filePath(name);
    if (!img.save(path, "PNG"))
        return {};
    return path;
}

} // namespace

class TestThumbnailCache : public QObject
{
    Q_OBJECT

private slots:
    void missThenReadyThenHit();
    void writesShardedDiskFile();
    void secondCacheInstanceReadsFromDisk();
    void unreadableSourceStillEmitsReady();
    void rawLoaderFallbackIsUsed();
};

void TestThumbnailCache::missThenReadyThenHit()
{
    QTemporaryDir cacheDir, srcDir;
    const QString src = paintSource(srcDir, QStringLiteral("a.png"), {600, 400}, Qt::red);
    QVERIFY(!src.isEmpty());

    ThumbnailCache cache(cacheDir.path());
    QSignalSpy spy(&cache, &ThumbnailCache::ready);

    QVERIFY(cache.thumbnail(QStringLiteral("hashA"), src, ThumbnailCache::kGridPx).isNull());
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("hashA"));

    const QPixmap hit = cache.thumbnail(QStringLiteral("hashA"), src, ThumbnailCache::kGridPx);
    QVERIFY(!hit.isNull());
    QVERIFY(qMax(hit.width(), hit.height()) <= ThumbnailCache::kGridPx);
    QCOMPARE(qMax(hit.width(), hit.height()), ThumbnailCache::kGridPx);
}

void TestThumbnailCache::writesShardedDiskFile()
{
    QTemporaryDir cacheDir, srcDir;
    const QString src = paintSource(srcDir, QStringLiteral("b.png"), {300, 300}, Qt::green);

    ThumbnailCache cache(cacheDir.path());
    QSignalSpy spy(&cache, &ThumbnailCache::ready);
    cache.thumbnail(QStringLiteral("abcdef123"), src, ThumbnailCache::kGridPx);
    QVERIFY(spy.wait(5000));

    const QString expected = cacheDir.filePath(
        QStringLiteral("%1/ab/abcdef123.jpg").arg(ThumbnailCache::kGridPx));
    QVERIFY2(QFileInfo::exists(expected), qPrintable(expected));
    QVERIFY(cache.isCached(QStringLiteral("abcdef123"), ThumbnailCache::kGridPx));
}

void TestThumbnailCache::secondCacheInstanceReadsFromDisk()
{
    QTemporaryDir cacheDir, srcDir;
    const QString src = paintSource(srcDir, QStringLiteral("c.png"), {400, 200}, Qt::blue);

    {
        ThumbnailCache warm(cacheDir.path());
        QSignalSpy spy(&warm, &ThumbnailCache::ready);
        warm.thumbnail(QStringLiteral("hashC"), src, ThumbnailCache::kGridPx);
        QVERIFY(spy.wait(5000));
    }

    ThumbnailCache cold(cacheDir.path());
    const QPixmap pm = cold.thumbnail(QStringLiteral("hashC"), src, ThumbnailCache::kGridPx);
    QVERIFY(!pm.isNull());   // straight from disk, no wait
}

void TestThumbnailCache::unreadableSourceStillEmitsReady()
{
    QTemporaryDir cacheDir;
    ThumbnailCache cache(cacheDir.path());
    QSignalSpy spy(&cache, &ThumbnailCache::ready);

    cache.thumbnail(QStringLiteral("hashMissing"),
                    QStringLiteral("/no/such/file.jpg"), ThumbnailCache::kGridPx);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 1);
    QVERIFY(!cache.isCached(QStringLiteral("hashMissing"), ThumbnailCache::kGridPx));
}

void TestThumbnailCache::rawLoaderFallbackIsUsed()
{
    QTemporaryDir cacheDir;
    ThumbnailCache cache(cacheDir.path());
    bool loaderCalled = false;
    cache.setRawLoader([&](const QString &, int edge) {
        loaderCalled = true;
        QImage img(edge, edge, QImage::Format_RGB32);
        img.fill(Qt::magenta);
        return img;
    });

    QSignalSpy spy(&cache, &ThumbnailCache::ready);
    cache.thumbnail(QStringLiteral("hashRaw"),
                    QStringLiteral("/tmp/does-not-exist.nef"), ThumbnailCache::kGridPx);
    QVERIFY(spy.wait(5000));

    QVERIFY(loaderCalled);
    QVERIFY(cache.isCached(QStringLiteral("hashRaw"), ThumbnailCache::kGridPx));
}

QTEST_MAIN(TestThumbnailCache)
#include "tst_thumbnailcache.moc"
