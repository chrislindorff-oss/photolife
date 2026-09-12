#include <QtTest>

#include "geo/PinClusterer.h"

using namespace pl::geo;

class TestPinClusterer : public QObject
{
    Q_OBJECT

private slots:
    void emptyInput();
    void singlePin();
    void nearbyPinsMergeAtLowZoom();
    void distantPinsStaySeparate();
    void zoomingInSplitsAMergedCluster();
    void allRowsArePreserved();
};

void TestPinClusterer::emptyInput()
{
    QVERIFY(clusterPins({}, 5).isEmpty());
}

void TestPinClusterer::singlePin()
{
    const QList<Pin> pins{{144.0, -37.0, 7}};
    const auto clusters = clusterPins(pins, 10);
    QCOMPARE(clusters.size(), 1);
    QCOMPARE(clusters.first().rows, (QList<int>{7}));
}

void TestPinClusterer::nearbyPinsMergeAtLowZoom()
{
    // Two points ~1m apart -- at a zoomed-out view they must collapse to one pin.
    const QList<Pin> pins{{144.24643, -37.87051, 0}, {144.24644, -37.87052, 1}};
    const auto clusters = clusterPins(pins, 3);
    QCOMPARE(clusters.size(), 1);
    QCOMPARE(clusters.first().rows.size(), 2);
}

void TestPinClusterer::distantPinsStaySeparate()
{
    // Melbourne vs. Seattle -- must never merge, at any reasonable zoom.
    const QList<Pin> pins{{144.9631, -37.8136, 0}, {-122.3321, 47.6062, 1}};
    const auto clusters = clusterPins(pins, 4);
    QCOMPARE(clusters.size(), 2);
}

void TestPinClusterer::zoomingInSplitsAMergedCluster()
{
    // Two points ~500m apart: should merge when the whole world is small on
    // screen (low zoom) and separate once zoomed in enough that 500m is wide.
    const QList<Pin> pins{{144.2460, -37.8700, 0}, {144.2510, -37.8700, 1}};
    QCOMPARE(clusterPins(pins, 5).size(), 1);
    QCOMPARE(clusterPins(pins, 16).size(), 2);
}

void TestPinClusterer::allRowsArePreserved()
{
    const QList<Pin> pins{{0.0, 0.0, 0}, {0.0, 0.0, 1}, {50.0, 50.0, 2}};
    const auto clusters = clusterPins(pins, 8);
    int total = 0;
    for (const Cluster &c : clusters)
        total += c.rows.size();
    QCOMPARE(total, pins.size());
}

QTEST_APPLESS_MAIN(TestPinClusterer)
#include "tst_pinclusterer.moc"
