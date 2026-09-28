#include <QtTest>

#include "geo/GeoShape.h"

using namespace pl::geo;

class TestGeoShape : public QObject
{
    Q_OBJECT

private slots:
    void parsesPolygon();
    void multiPolygonPartsAndHoles();
    void unwrapsAcrossAntimeridian();
    void rejectsOtherGeometry();
};

void TestGeoShape::parsesPolygon()
{
    const auto shape = parseGeoJsonShape(QStringLiteral(
        R"({"type":"Polygon","coordinates":[[[140,-39],[150,-39],[150,-34],[140,-34],[140,-39]]]})"));
    QVERIFY(shape);
    QCOMPARE(shape->rings.size(), 1);
    QVERIFY(shapeContains(*shape, 145, -37));
    QVERIFY(!shapeContains(*shape, 139, -37));
    QVERIFY(!shapeContains(*shape, 145, -33));
}

void TestGeoShape::multiPolygonPartsAndHoles()
{
    // A square with a square hole, plus a separate island.
    const auto shape = parseGeoJsonShape(QStringLiteral(R"({"type":"MultiPolygon","coordinates":[
        [[[0,0],[10,0],[10,10],[0,10],[0,0]], [[4,4],[6,4],[6,6],[4,6],[4,4]]],
        [[[20,0],[22,0],[22,2],[20,2],[20,0]]]]})"));
    QVERIFY(shape);
    QCOMPARE(shape->rings.size(), 3);
    QVERIFY(shapeContains(*shape, 1, 1));
    QVERIFY(!shapeContains(*shape, 5, 5));    // in the hole
    QVERIFY(shapeContains(*shape, 21, 1));    // the island
    QVERIFY(!shapeContains(*shape, 15, 1));   // between them
}

void TestGeoShape::unwrapsAcrossAntimeridian()
{
    // 178°E to 178°W: one ring, not a band around the world.
    const auto shape = parseGeoJsonShape(QStringLiteral(
        R"({"type":"Polygon","coordinates":[[[178,-18],[-178,-18],[-178,-16],[178,-16],[178,-18]]]})"));
    QVERIFY(shape);
    QCOMPARE(shape->rings.first().at(1).x(), 182.0);
    QVERIFY(shapeContains(*shape, 179.5, -17));
    QVERIFY(shapeContains(*shape, -179.5, -17));
    QVERIFY(!shapeContains(*shape, 0, -17));
    QVERIFY(!shapeContains(*shape, 170, -17));
}

void TestGeoShape::rejectsOtherGeometry()
{
    QVERIFY(!parseGeoJsonShape(QStringLiteral(R"({"type":"Point","coordinates":[1,2]})")));
    QVERIFY(!parseGeoJsonShape(QStringLiteral("not json")));
    QVERIFY(!parseGeoJsonShape(QString()));
    // A degenerate ring (under three points) is dropped.
    QVERIFY(!parseGeoJsonShape(
        QStringLiteral(R"({"type":"Polygon","coordinates":[[[0,0],[1,1]]]})")));
}

QTEST_GUILESS_MAIN(TestGeoShape)
#include "tst_geoshape.moc"
