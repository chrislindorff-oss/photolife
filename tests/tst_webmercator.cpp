#include <QtTest>

#include "geo/WebMercator.h"

using namespace pl::geo;

class TestWebMercator : public QObject
{
    Q_OBJECT

private slots:
    void originAtZoomZero();
    void worldEdgesAtZoomZero();
    void roundTrip();
    void higherZoomDoublesWorldSize();
};

void TestWebMercator::originAtZoomZero()
{
    // (0, 0) lon/lat is the center of the projected world at any zoom.
    const QPointF p = lonLatToWorldPixel(0.0, 0.0, 0);
    QCOMPARE(p.x(), kTileSize / 2.0);
    QCOMPARE(p.y(), kTileSize / 2.0);
}

void TestWebMercator::worldEdgesAtZoomZero()
{
    const QPointF left = lonLatToWorldPixel(-180.0, 0.0, 0);
    const QPointF right = lonLatToWorldPixel(180.0, 0.0, 0);
    QCOMPARE(left.x(), 0.0);
    QCOMPARE(right.x(), double(kTileSize));
}

void TestWebMercator::roundTrip()
{
    for (const auto &lonLat : {QPointF{144.246, -37.870}, QPointF{0.0, 0.0}, QPointF{-122.4, 47.6},
                              QPointF{179.9, -85.0}}) {
        for (int zoom : {0, 3, 10, 18}) {
            const QPointF px = lonLatToWorldPixel(lonLat.x(), lonLat.y(), zoom);
            const QPointF back = worldPixelToLonLat(px, zoom);
            QVERIFY2(std::abs(back.x() - lonLat.x()) < 1e-6,
                     qPrintable(QStringLiteral("lon at zoom %1").arg(zoom)));
            QVERIFY2(std::abs(back.y() - lonLat.y()) < 1e-6,
                     qPrintable(QStringLiteral("lat at zoom %1").arg(zoom)));
        }
    }
}

void TestWebMercator::higherZoomDoublesWorldSize()
{
    const QPointF a = lonLatToWorldPixel(90.0, 0.0, 4);
    const QPointF b = lonLatToWorldPixel(90.0, 0.0, 5);
    // Going up one zoom level doubles the world pixel size, so any fixed
    // lon/lat's distance from the origin (0,0) doubles too.
    QCOMPARE(b.x(), a.x() * 2.0);
}

QTEST_APPLESS_MAIN(TestWebMercator)
#include "tst_webmercator.moc"
