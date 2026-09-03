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
    void inactiveTaxon();
    void placeWithBoundingBox();
    void placeWithNullAdminLevel();
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

QTEST_APPLESS_MAIN(TestINatParse)
#include "tst_inatparse.moc"
