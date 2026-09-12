#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "net/INatParse.h"

using namespace pl::net::inat;

namespace {
QJsonObject obj(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}
} // namespace

class TestINatParse : public QObject
{
    Q_OBJECT

private slots:
    void ancestrySplits();
    void taxonCoreFields();
    void taxonParentFromAncestryWhenNoParentId();
    void taxonNamesSplitIntoSynonymsAndVernacular();
    void taxonDefaultPhoto();
    void inactiveTaxon();
    void placeWithBoundingBox();
    void placeWithNullAdminLevel();
    void observationCoreFieldsAndExplicitPhotoSizes();
    void observationDerivesOriginalFromSquareUrl();
    void observationWithNoCoordinatesLeavesLatLonUnset();
    void observationParsesPlaceGuess();
};

void TestINatParse::ancestrySplits()
{
    QCOMPARE(ancestryIds(QStringLiteral("48460/47126/47217")),
             (QList<qint64>{48460, 47126, 47217}));
    QVERIFY(ancestryIds(QString()).isEmpty());
}

void TestINatParse::taxonCoreFields()
{
    const auto t = parseTaxon(obj(R"({
        "id": 47217, "rank": "family", "rank_level": 30, "name": "Orchidaceae",
        "preferred_common_name": "Orchids", "parent_id": 47218,
        "ancestry": "48460/47126/211194/47125/47163/47218", "is_active": true
    })"));
    QCOMPARE(t.inatId, qint64(47217));
    QCOMPARE(t.rank, QStringLiteral("family"));
    QCOMPARE(t.rankLevel.value_or(-1), 30);
    QCOMPARE(t.name, QStringLiteral("Orchidaceae"));
    QCOMPARE(t.commonName, QStringLiteral("Orchids"));
    QCOMPARE(t.parentInatId.value_or(-1), qint64(47218));
    QVERIFY(t.vernacular.contains(QStringLiteral("Orchids")));
}

void TestINatParse::taxonParentFromAncestryWhenNoParentId()
{
    const auto t = parseTaxon(obj(R"({
        "id": 5, "rank": "genus", "name": "Diuris", "ancestry": "1/2/3"
    })"));
    QCOMPARE(t.parentInatId.value_or(-1), qint64(3));
}

void TestINatParse::taxonNamesSplitIntoSynonymsAndVernacular()
{
    const auto t = parseTaxon(obj(R"({
        "id": 10, "rank": "species", "name": "Caladenia carnea",
        "names": [
            {"name": "Caladenia carnea", "lexicon": "scientific-names"},
            {"name": "Petalochilus carneus", "lexicon": "scientific-names"},
            {"name": "Pink Fingers", "lexicon": "english"},
            {"name": "Pink Lady Fingers", "lexicon": "English"}
        ]
    })"));
    QCOMPARE(t.synonyms, QStringList{QStringLiteral("Petalochilus carneus")});
    QVERIFY(t.vernacular.contains(QStringLiteral("Pink Fingers")));
    QVERIFY(t.vernacular.contains(QStringLiteral("Pink Lady Fingers")));
    QVERIFY(!t.synonyms.contains(QStringLiteral("Caladenia carnea")));
}

void TestINatParse::taxonDefaultPhoto()
{
    const auto t = parseTaxon(obj(R"json({
        "id": 10, "rank": "species", "name": "Caladenia carnea",
        "default_photo": {
            "square_url": "https://inat.example/photos/1/square.jpg",
            "medium_url": "https://inat.example/photos/1/medium.jpg",
            "attribution": "(c) someone, some rights reserved (CC BY-NC)"
        }
    })json"));
    QCOMPARE(t.photoUrl, QStringLiteral("https://inat.example/photos/1/medium.jpg"));
    QCOMPARE(t.photoAttribution,
             QStringLiteral("(c) someone, some rights reserved (CC BY-NC)"));

    // Falls back to a smaller size when medium_url is absent.
    const auto small = parseTaxon(obj(R"({
        "id": 11, "rank": "species", "name": "X",
        "default_photo": {"square_url": "https://inat.example/photos/2/square.jpg"}
    })"));
    QCOMPARE(small.photoUrl, QStringLiteral("https://inat.example/photos/2/square.jpg"));

    // No default_photo -> empty, not a crash.
    const auto none = parseTaxon(obj(R"({"id": 12, "rank": "species", "name": "Y"})"));
    QVERIFY(none.photoUrl.isEmpty());
}

void TestINatParse::inactiveTaxon()
{
    const auto t = parseTaxon(obj(R"({"id": 1, "rank": "species", "name": "X", "is_active": false})"));
    QVERIFY(!t.isActive);
}

void TestINatParse::placeWithBoundingBox()
{
    const auto p = parsePlace(obj(R"({
        "id": 6744, "name": "Victoria", "display_name": "Victoria, AU", "admin_level": 10,
        "bounding_box_geojson": {"type": "Polygon", "coordinates": [[
            [140.9, -39.2], [140.9, -33.9], [150.0, -33.9], [150.0, -39.2], [140.9, -39.2]
        ]]}
    })"));
    QCOMPARE(p.inatId, qint64(6744));
    QCOMPARE(p.displayName, QStringLiteral("Victoria, AU"));
    QCOMPARE(p.adminLevel.value_or(-1), 10);
    QVERIFY(p.bboxSwLat.has_value());
    QCOMPARE(p.bboxSwLat.value(), -39.2);
    QCOMPARE(p.bboxNeLng.value(), 150.0);
}

void TestINatParse::placeWithNullAdminLevel()
{
    const auto p = parsePlace(obj(R"({"id": 7, "name": "Gabo Island", "admin_level": null})"));
    QVERIFY(!p.adminLevel.has_value());
}

void TestINatParse::observationCoreFieldsAndExplicitPhotoSizes()
{
    const auto o = parseObservation(obj(R"({
        "id": 987654, "observed_on": "2025-11-02",
        "taxon": {"id": 60815},
        "geojson": {"type": "Point", "coordinates": [144.9631, -37.8136]},
        "photos": [
            {"id": 1, "square_url": "https://x/1/square.jpg",
             "medium_url": "https://x/1/medium.jpg"}
        ]
    })"));
    QCOMPARE(o.id, qint64(987654));
    QCOMPARE(o.observedOn, QStringLiteral("2025-11-02"));
    QCOMPARE(o.taxonInatId, qint64(60815));
    QVERIFY(o.longitude.has_value());
    QCOMPARE(o.longitude.value(), 144.9631);
    QCOMPARE(o.latitude.value(), -37.8136);
    QCOMPARE(o.photos.size(), 1);
    QCOMPARE(o.photos.first().id, qint64(1));
    QCOMPARE(o.photos.first().previewUrl, QStringLiteral("https://x/1/square.jpg"));
    QCOMPARE(o.photos.first().downloadUrl, QStringLiteral("https://x/1/medium.jpg"));
}

void TestINatParse::observationDerivesOriginalFromSquareUrl()
{
    const auto o = parseObservation(obj(R"({
        "id": 1, "taxon": {"id": 2},
        "photos": [{"id": 5, "url": "https://x/5/square.jpg"}]
    })"));
    QCOMPARE(o.photos.size(), 1);
    // No explicit sized fields at all -> preview stays the cheap default,
    // download is derived up to "original" from it.
    QCOMPARE(o.photos.first().previewUrl, QStringLiteral("https://x/5/square.jpg"));
    QCOMPARE(o.photos.first().downloadUrl, QStringLiteral("https://x/5/original.jpg"));
}

void TestINatParse::observationWithNoCoordinatesLeavesLatLonUnset()
{
    const auto o = parseObservation(obj(R"({"id": 1, "taxon": {"id": 2}})"));
    QVERIFY(!o.latitude.has_value());
    QVERIFY(!o.longitude.has_value());
    QVERIFY(o.photos.isEmpty());
}

void TestINatParse::observationParsesPlaceGuess()
{
    const auto withPlace = parseObservation(obj(
        R"({"id": 1, "taxon": {"id": 2}, "place_guess": "Fitzroy Gardens, Melbourne, VIC"})"));
    QCOMPARE(withPlace.placeGuess, QStringLiteral("Fitzroy Gardens, Melbourne, VIC"));

    const auto withoutPlace = parseObservation(obj(R"({"id": 1, "taxon": {"id": 2}})"));
    QVERIFY(withoutPlace.placeGuess.isEmpty());
}

QTEST_APPLESS_MAIN(TestINatParse)
#include "tst_inatparse.moc"
