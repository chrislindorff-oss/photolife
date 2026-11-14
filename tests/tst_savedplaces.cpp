#include <QtTest>

#include "geo/SavedPlaces.h"

using namespace pl::geo;

class TestSavedPlaces : public QObject
{
    Q_OBJECT

private slots:
    void storesAndReadsBack();
    void replacingKeepsOnePerNameInOrder();
    void placeNamesMayHaveSpaces();
    void longestSavedNameWins();
    void quotedNamesAndUnknownOnes();
    void noPlaceLeavesTheSearchAlone();
    void boxClauseHandlesTheDateLine();
};

namespace {
QList<SavedPlace> places()
{
    return {{QStringLiteral("Anakie"), {-37.95, 144.15, -37.85, 144.30}},
            {QStringLiteral("Anakie Gorge"), {-37.92, 144.22, -37.89, 144.26}},
            {QStringLiteral("Jollys Rd"), {-37.90, 144.10, -37.88, 144.13}}};
}
} // namespace

void TestSavedPlaces::storesAndReadsBack()
{
    const QStringList stored = formatSavedPlaces(places());
    QCOMPARE(stored.size(), 3);
    const QList<SavedPlace> back = parseSavedPlaces(stored + QStringList{QStringLiteral("broken\tline")});
    QCOMPARE(back.size(), 3);
    QCOMPARE(back.at(1).name, QStringLiteral("Anakie Gorge"));
    QCOMPARE(back.at(1).box.swLat, -37.92);
    QCOMPARE(back.at(1).box.neLng, 144.26);
}

void TestSavedPlaces::replacingKeepsOnePerNameInOrder()
{
    const QList<SavedPlace> next = withSavedPlace(places(), {QStringLiteral("anakie"), {-1, -1, 1, 1}});
    QCOMPARE(next.size(), 3);
    QCOMPARE(next.first().name, QStringLiteral("anakie"));
    QCOMPARE(next.first().box.neLat, 1.0);
    QVERIFY(findSavedPlace(next, QStringLiteral("ANAKIE")).has_value());
    QCOMPARE(withSavedPlace(places(), {QStringLiteral("Brisbane Ranges"), {}}).at(2).name,
             QStringLiteral("Brisbane Ranges"));
}

void TestSavedPlaces::placeNamesMayHaveSpaces()
{
    const PlaceQuery q = extractPlace(QStringLiteral("robin place:jollys rd 2025"), places());
    QVERIFY(q.place);
    QCOMPARE(q.place->name, QStringLiteral("Jollys Rd"));
    QCOMPARE(q.rest, QStringLiteral("robin 2025"));
    QVERIFY(q.unknown.isEmpty());
}

void TestSavedPlaces::longestSavedNameWins()
{
    PlaceQuery q = extractPlace(QStringLiteral("place:Anakie Gorge orchid"), places());
    QCOMPARE(q.place->name, QStringLiteral("Anakie Gorge"));
    QCOMPARE(q.rest, QStringLiteral("orchid"));
    q = extractPlace(QStringLiteral("place:Anakie Gorgeous"), places());   // whole words only
    QCOMPARE(q.place->name, QStringLiteral("Anakie"));
    QCOMPARE(q.rest, QStringLiteral("Gorgeous"));
}

void TestSavedPlaces::quotedNamesAndUnknownOnes()
{
    PlaceQuery q = extractPlace(QStringLiteral("place:\"anakie gorge\" orchid"), places());
    QCOMPARE(q.place->name, QStringLiteral("Anakie Gorge"));
    QCOMPARE(q.rest, QStringLiteral("orchid"));
    q = extractPlace(QStringLiteral("place:Nowhere orchid"), places());
    QVERIFY(!q.place);
    QCOMPARE(q.unknown, QStringLiteral("Nowhere"));
    QCOMPARE(q.rest, QStringLiteral("orchid"));
}

void TestSavedPlaces::noPlaceLeavesTheSearchAlone()
{
    const PlaceQuery q = extractPlace(QStringLiteral("  flame  robin "), places());
    QVERIFY(!q.place);
    QVERIFY(q.unknown.isEmpty());
    QCOMPARE(q.rest, QStringLiteral("flame robin"));
    // Only as a word of its own.
    QCOMPARE(extractPlace(QStringLiteral("myplace:x"), places()).rest, QStringLiteral("myplace:x"));
}

void TestSavedPlaces::boxClauseHandlesTheDateLine()
{
    const QString plain = boxClause({-38, 144, -37, 145}, QStringLiteral("lat"), QStringLiteral("lon"));
    QVERIFY(plain.contains(QStringLiteral("lat BETWEEN -38.0000000 AND -37.0000000")));
    QVERIFY(plain.contains(QStringLiteral("lon BETWEEN 144.0000000 AND 145.0000000")));
    const QString across = boxClause({-20, 179, -10, -179}, QStringLiteral("lat"), QStringLiteral("lon"));
    QVERIFY(across.contains(QStringLiteral("(lon >= 179.0000000 OR lon <= -179.0000000)")));
}

QTEST_MAIN(TestSavedPlaces)
#include "tst_savedplaces.moc"
