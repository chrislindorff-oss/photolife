#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "net/GeocodeParse.h"

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
