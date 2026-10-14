#include <QtTest>

#include "collection/FileNamePattern.h"

using namespace pl::collection;

namespace {
NameFields sampleFields()
{
    NameFields f;
    f.scientificName = QStringLiteral("Caladenia carnea");
    f.commonName = QStringLiteral("Pink Fingers");
    f.captured = QDateTime(QDate(2020, 1, 9), QTime(14, 5, 30));
    f.town = QStringLiteral("Anglesea");
    f.state = QStringLiteral("Victoria");
    f.country = QStringLiteral("Australia");
    f.author = QStringLiteral("C. Lee");
    f.originalStem = QStringLiteral("IMG_4821");
    return f;
}

FileNamePattern patternOf(const QList<NameToken> &tokens)
{
    FileNamePattern p;
    for (NameToken t : tokens)
        p.parts.append({t, {}});
    return p;
}
} // namespace

class TestFileNamePattern : public QObject
{
    Q_OBJECT

private slots:
    void rendersPartsInOrder();
    void leavesOutPartsWithNoValue();
    void replacesSpacesWithSeparatorWhenAsked();
    void makesPartsSafeForFilenames();
    void rendersDateFormatsAndTime();
    void rendersSequenceZeroPadded();
    void fallsBackToOriginalNameWhenNothingIsLeft();
    void capsLength();
    void roundTripsThroughJson();
    void ignoresUnknownTokensInJson();
    void splitsLocality_data();
    void splitsLocality();
    void parsesCapturedOn();
};

void TestFileNamePattern::rendersPartsInOrder()
{
    FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::DateTaken,
                                   NameToken::Town, NameToken::Author});
    p.parts.append({NameToken::Text, QStringLiteral("ref")});
    QCOMPARE(p.render(sampleFields()),
             QStringLiteral("Caladenia carnea_2020-01-09_Anglesea_C. Lee_ref"));

    p.separator = QStringLiteral(" - ");
    QCOMPARE(p.render(sampleFields()),
             QStringLiteral("Caladenia carnea - 2020-01-09 - Anglesea - C. Lee - ref"));
}

void TestFileNamePattern::leavesOutPartsWithNoValue()
{
    const FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::CommonName,
                                         NameToken::Town, NameToken::DateTaken});
    NameFields f = sampleFields();
    f.commonName.clear();
    f.town.clear();
    f.captured = {};
    QCOMPARE(p.render(f), QStringLiteral("Caladenia carnea"));
}

void TestFileNamePattern::replacesSpacesWithSeparatorWhenAsked()
{
    FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::CommonName});
    p.spacesToSeparator = true;
    QCOMPARE(p.render(sampleFields()), QStringLiteral("Caladenia_carnea_Pink_Fingers"));

    // A space separator has nothing to replace spaces with.
    p.separator = QStringLiteral(" ");
    QCOMPARE(p.render(sampleFields()), QStringLiteral("Caladenia carnea Pink Fingers"));
}

void TestFileNamePattern::makesPartsSafeForFilenames()
{
    FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::Town});
    NameFields f = sampleFields();
    f.scientificName = QStringLiteral("Genus/species: \"x\"");
    f.town = QStringLiteral("A?B");
    QCOMPARE(p.render(f), QStringLiteral("Genus species x_A B"));

    p.separator = QStringLiteral(":");   // an unsafe separator is dropped, not kept
    QCOMPARE(p.render(f), QStringLiteral("Genus species xA B"));
}

void TestFileNamePattern::rendersDateFormatsAndTime()
{
    FileNamePattern p = patternOf({NameToken::DateTaken});
    const NameFields f = sampleFields();
    QCOMPARE(p.render(f), QStringLiteral("2020-01-09"));
    p.dateFormat = QStringLiteral("yyyyMMdd");
    QCOMPARE(p.render(f), QStringLiteral("20200109"));
    p.dateFormat = QStringLiteral("dd-MM-yyyy");
    QCOMPARE(p.render(f), QStringLiteral("09-01-2020"));

    QCOMPARE(patternOf({NameToken::Year, NameToken::TimeTaken}).render(f),
             QStringLiteral("2020_140530"));
}

void TestFileNamePattern::rendersSequenceZeroPadded()
{
    const FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::Sequence});
    NameFields f = sampleFields();
    QCOMPARE(p.render(f), QStringLiteral("Caladenia carnea"));   // 0 = left out
    f.sequence = 7;
    QCOMPARE(p.render(f), QStringLiteral("Caladenia carnea_007"));
    f.sequence = 1234;
    QCOMPARE(p.render(f), QStringLiteral("Caladenia carnea_1234"));
}

void TestFileNamePattern::fallsBackToOriginalNameWhenNothingIsLeft()
{
    const FileNamePattern p = patternOf({NameToken::CommonName, NameToken::Town});
    NameFields f = sampleFields();
    f.commonName.clear();
    f.town.clear();
    QCOMPARE(p.render(f), QStringLiteral("IMG_4821"));
    QVERIFY(p.renderParts(f).isEmpty());
}

void TestFileNamePattern::capsLength()
{
    FileNamePattern p;
    p.parts.append({NameToken::Text, QString(500, QLatin1Char('x'))});
    QCOMPARE(p.render(sampleFields()).size(), FileNamePattern::kMaxStemLength);
}

void TestFileNamePattern::roundTripsThroughJson()
{
    FileNamePattern p = patternOf({NameToken::ScientificName, NameToken::DateTaken,
                                   NameToken::Sequence});
    p.parts.insert(1, {NameToken::Text, QStringLiteral("ref")});
    p.separator = QStringLiteral("-");
    p.dateFormat = QStringLiteral("yyyyMMdd");
    p.spacesToSeparator = true;
    QCOMPARE(FileNamePattern::fromJson(p.toJson()), p);

    QVERIFY(FileNamePattern::fromJson(QJsonObject()).isEmpty());
}

void TestFileNamePattern::ignoresUnknownTokensInJson()
{
    QJsonObject unknown{{QStringLiteral("token"), QStringLiteral("fromTheFuture")}};
    QJsonObject known{{QStringLiteral("token"), QStringLiteral("scientificName")}};
    QJsonObject json{{QStringLiteral("parts"), QJsonArray{unknown, known}}};
    const FileNamePattern p = FileNamePattern::fromJson(json);
    QCOMPARE(p.parts.size(), 1);
    QCOMPARE(p.parts.first().token, NameToken::ScientificName);
}

void TestFileNamePattern::splitsLocality_data()
{
    QTest::addColumn<QString>("locality");
    QTest::addColumn<QString>("town");
    QTest::addColumn<QString>("state");
    QTest::addColumn<QString>("country");
    QTest::newRow("all three") << "Anglesea, Victoria, Australia" << "Anglesea" << "Victoria"
                               << "Australia";
    QTest::newRow("no town") << "Victoria, Australia" << "" << "Victoria" << "Australia";
    QTest::newRow("country only") << "Australia" << "" << "" << "Australia";
    QTest::newRow("empty") << "" << "" << "" << "";
}

void TestFileNamePattern::splitsLocality()
{
    QFETCH(QString, locality);
    const LocalityParts parts = pl::collection::splitLocality(locality);
    QTEST(parts.town, "town");
    QTEST(parts.state, "state");
    QTEST(parts.country, "country");
}

void TestFileNamePattern::parsesCapturedOn()
{
    QCOMPARE(parseCapturedOn(QStringLiteral("2020-01-09T14:05:30")),
             QDateTime(QDate(2020, 1, 9), QTime(14, 5, 30)));
    QCOMPARE(parseCapturedOn(QStringLiteral("2020-01-09")).date(), QDate(2020, 1, 9));
    QVERIFY(!parseCapturedOn(QString()).isValid());
}

QTEST_GUILESS_MAIN(TestFileNamePattern)
#include "tst_filenamepattern.moc"
