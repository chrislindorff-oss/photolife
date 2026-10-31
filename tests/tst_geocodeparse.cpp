#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "net/GeocodeParse.h"
#include "ui/GeotagDialog.h"

#include <QPointF>

using namespace pl::net::geocode;

namespace {
QJsonObject obj(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}
} // namespace

class TestGeocodeParse : public QObject
{
    Q_OBJECT

private slots:
    void fullAddressJoinsTownStateCountry();
    void fallsBackThroughTownKeys();
    void awayFromTownsTheLocalityIsUsed();
    void searchResultsAreParsed();
    void pastedCoordinatesAreRead();
    void noAddressIsEmpty();
    void errorResponseIsEmpty();
};

void TestGeocodeParse::fullAddressJoinsTownStateCountry()
{
    const QString locality = formatLocality(obj(R"({
        "address": {
            "city": "Warburton", "state": "Victoria", "country": "Australia",
            "postcode": "3799", "country_code": "au"
        }
    })"));
    QCOMPARE(locality, QStringLiteral("Warburton, Victoria, Australia"));
}

void TestGeocodeParse::fallsBackThroughTownKeys()
{
    QCOMPARE(formatLocality(obj(R"({"address": {"town": "Healesville", "country": "Australia"}})")),
             QStringLiteral("Healesville, Australia"));
    QCOMPARE(formatLocality(obj(R"({"address": {"village": "Toolangi", "country": "Australia"}})")),
             QStringLiteral("Toolangi, Australia"));
    QCOMPARE(formatLocality(obj(R"({"address": {"hamlet": "Yarra Junction"}})")),
             QStringLiteral("Yarra Junction"));
}

void TestGeocodeParse::awayFromTownsTheLocalityIsUsed()
{
    // A walking track in a national park: OpenStreetMap names the gazetted
    // locality as a city_district...
    QCOMPARE(formatLocality(obj(R"({"address": {"road": "Walk 18 - Bymien to Poona Lake",
        "city_district": "Cooloola", "state": "Queensland", "postcode": "4580",
        "country": "Australia"}})")),
             QStringLiteral("Cooloola, Queensland, Australia"));
    // ...or as a locality.
    QCOMPARE(formatLocality(obj(R"({"address": {"locality": "Neds Corner", "state": "Victoria",
        "country": "Australia"}})")),
             QStringLiteral("Neds Corner, Victoria, Australia"));
    // A town still comes first.
    QCOMPARE(formatLocality(obj(R"({"address": {"town": "Halls Gap", "city_district": "Grampians",
        "state": "Victoria", "country": "Australia"}})")),
             QStringLiteral("Halls Gap, Victoria, Australia"));
    // Just a road: no locality.
    QCOMPARE(formatLocality(obj(R"({"address": {"road": "Switch Road", "state": "Victoria",
        "country": "Australia"}})")),
             QStringLiteral("Victoria, Australia"));
}

void TestGeocodeParse::searchResultsAreParsed()
{
    const QJsonArray response = QJsonDocument::fromJson(R"([
        {"lat": "-25.9542", "lon": "153.1045", "display_name": "Poona Lake, Cooloola, Queensland, Australia"},
        {"lat": "bad", "lon": "1", "display_name": "Broken"},
        {"lat": "-37.81", "lon": "144.96", "display_name": "Melbourne, Victoria, Australia"}
    ])").array();
    const QList<Place> places = parseSearch(response);
    QCOMPARE(places.size(), 2);
    QCOMPARE(places.first().name, QStringLiteral("Poona Lake, Cooloola, Queensland, Australia"));
    QVERIFY(qAbs(places.first().latitude - -25.9542) < 1e-9);
    QVERIFY(qAbs(places.last().longitude - 144.96) < 1e-9);
}

void TestGeocodeParse::pastedCoordinatesAreRead()
{
    using pl::GeotagDialog;
    QCOMPARE(GeotagDialog::parseCoordinates(QStringLiteral("-25.954, 153.105")), QPointF(-25.954, 153.105));
    QCOMPARE(GeotagDialog::parseCoordinates(QStringLiteral(" -25.954 153.105 ")), QPointF(-25.954, 153.105));
    QCOMPARE(GeotagDialog::parseCoordinates(QStringLiteral("25.954 S, 153.105 E")), QPointF(-25.954, 153.105));
    QCOMPARE(GeotagDialog::parseCoordinates(QStringLiteral("51.5°N, 0.1278°W")), QPointF(51.5, -0.1278));
    QVERIFY(!GeotagDialog::parseCoordinates(QStringLiteral("Poona Lake")));
    QVERIFY(!GeotagDialog::parseCoordinates(QStringLiteral("95, 10")));   // no such latitude
}

void TestGeocodeParse::noAddressIsEmpty()
{
    QVERIFY(formatLocality(obj(R"({"place_id": 1})")).isEmpty());
    QVERIFY(formatLocality(obj(R"({"address": {}})")).isEmpty());
}

void TestGeocodeParse::errorResponseIsEmpty()
{
    QVERIFY(formatLocality(obj(R"({"error": "Unable to geocode"})")).isEmpty());
}

QTEST_APPLESS_MAIN(TestGeocodeParse)
#include "tst_geocodeparse.moc"
