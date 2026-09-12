#include <QtTest>

#include "geo/GeoBox.h"

using namespace pl::geo;

class TestGeoBox : public QObject
{
    Q_OBJECT

private slots:
    void pointInsideIsContained();
    void pointOutsideEachSideIsNotContained();
    void pointsOnEdgesAreContained();
};

namespace {
// Roughly Victoria, Australia's extent.
const GeoBox kVictoria{-39.2, 140.9, -33.9, 150.0};
} // namespace

void TestGeoBox::pointInsideIsContained()
{
    QVERIFY(boxContains(kVictoria, 144.9631, -37.8136));   // Melbourne
}

void TestGeoBox::pointOutsideEachSideIsNotContained()
{
    QVERIFY(!boxContains(kVictoria, 144.9631, -33.0));   // north of the box
    QVERIFY(!boxContains(kVictoria, 144.9631, -40.0));   // south of the box
    QVERIFY(!boxContains(kVictoria, 151.2093, -37.8136)); // east of the box (Sydney)
    QVERIFY(!boxContains(kVictoria, 138.6007, -37.8136)); // west of the box (Adelaide)
}

void TestGeoBox::pointsOnEdgesAreContained()
{
    QVERIFY(boxContains(kVictoria, kVictoria.swLng, kVictoria.swLat));
    QVERIFY(boxContains(kVictoria, kVictoria.neLng, kVictoria.neLat));
}

QTEST_APPLESS_MAIN(TestGeoBox)
#include "tst_geobox.moc"
