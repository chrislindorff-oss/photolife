#include <QtTest>

#include "scan/FilenameParser.h"

using namespace pl::scan;

class TestFilenameParser : public QObject
{
    Q_OBJECT

private slots:
    void canonicalFloraName();
    void organTagInParentheses();
    void underscoreOrganSuffix();
    void faunaCommonNamePairIsNotAnOrganTag();
    void enDashSeparator();
    void trailingUnderscoreSuffixAfterDate();
    void dayMonthYearIsPreferredReading();
    void ambiguousDateFallsBackToMonthDay();
    void noSeparatorKeepsWholeStemAsName();
    void duplicateCounterWithoutDate();
    void synonymGenusPairSurvives();
    void uncertaintyMarkersAreLeftInName();
    void baseNameStripsOnlyFinalExtension();
    void emptyStem();
};

void TestFilenameParser::canonicalFloraName()
{
    const ParsedFilename p = parseStem(
        QStringLiteral("Diuris pardina - 401 Fulbrooks Road, Dadswells Bridge 28-9-2020 (1)"));
    QCOMPARE(p.name, QStringLiteral("Diuris pardina"));
    QCOMPARE(p.locality, QStringLiteral("401 Fulbrooks Road, Dadswells Bridge"));
    QCOMPARE(p.capturedOn, QDate(2020, 9, 28));
    QCOMPARE(p.sequence, 1);
    QVERIFY(p.organTags.isEmpty());
}

void TestFilenameParser::organTagInParentheses()
{
    const ParsedFilename p = parseStem(
        QStringLiteral("Caladenia carnea (bud) - Anglesea 12-10-2019"));
    QCOMPARE(p.name, QStringLiteral("Caladenia carnea"));
    QCOMPARE(p.organTags, QStringList{QStringLiteral("bud")});
    QCOMPARE(p.capturedOn, QDate(2019, 10, 12));
}

void TestFilenameParser::underscoreOrganSuffix()
{
    const ParsedFilename p = parseStem(
        QStringLiteral("Acacia dealbata_bark - Kinglake 3-8-2021"));
    QCOMPARE(p.name, QStringLiteral("Acacia dealbata"));
    QCOMPARE(p.organTags, QStringList{QStringLiteral("bark")});
}

void TestFilenameParser::faunaCommonNamePairIsNotAnOrganTag()
{
    const ParsedFilename p = parseStem(
        QStringLiteral("Jacky Winter_White-winged Triller - Terrick Terrick 5-11-2018"));
    QCOMPARE(p.name, QStringLiteral("Jacky Winter_White-winged Triller"));
    QVERIFY(p.organTags.isEmpty());
    QCOMPARE(p.capturedOn, QDate(2018, 11, 5));
}

void TestFilenameParser::enDashSeparator()
{
    const ParsedFilename p = parseStem(
        QString::fromUtf8("Petalochilus carneus \xE2\x80\x93 Brisbane Ranges 1-9-2016"));
    QCOMPARE(p.name, QStringLiteral("Petalochilus carneus"));
    QCOMPARE(p.locality, QStringLiteral("Brisbane Ranges"));
    QCOMPARE(p.capturedOn, QDate(2016, 9, 1));
}

void TestFilenameParser::trailingUnderscoreSuffixAfterDate()
{
    const ParsedFilename p = parseStem(QStringLiteral(
        "White-bellied Sea-eagle - Western Treatment Plant, Werribee 21-03-2026_41-079"));
    QCOMPARE(p.name, QStringLiteral("White-bellied Sea-eagle"));
    QCOMPARE(p.locality, QStringLiteral("Western Treatment Plant, Werribee"));
    QCOMPARE(p.capturedOn, QDate(2026, 3, 21));
}

void TestFilenameParser::dayMonthYearIsPreferredReading()
{
    const ParsedFilename p = parseStem(QStringLiteral("Genus species - Somewhere 4-5-2020"));
    QCOMPARE(p.capturedOn, QDate(2020, 5, 4));  // 4 May, not 5 April
}

void TestFilenameParser::ambiguousDateFallsBackToMonthDay()
{
    // 25 as a month is invalid, so this can only be month=7, day=25.
    const ParsedFilename p = parseStem(QStringLiteral("Genus species - Somewhere 7-25-2020"));
    QCOMPARE(p.capturedOn, QDate(2020, 7, 25));
}

void TestFilenameParser::noSeparatorKeepsWholeStemAsName()
{
    const ParsedFilename p = parseStem(QStringLiteral("Unidentified Caladenia"));
    QCOMPARE(p.name, QStringLiteral("Unidentified Caladenia"));
    QVERIFY(p.locality.isEmpty());
    QVERIFY(!p.capturedOn.isValid());
}

void TestFilenameParser::duplicateCounterWithoutDate()
{
    const ParsedFilename p = parseStem(QStringLiteral("Diuris sp (3)"));
    QCOMPARE(p.name, QStringLiteral("Diuris sp"));
    QCOMPARE(p.sequence, 3);
}

void TestFilenameParser::synonymGenusPairSurvives()
{
    const ParsedFilename p = parseStem(
        QStringLiteral("Corybas_Corysanthes diemenica - Otways 20-6-2017"));
    QCOMPARE(p.name, QStringLiteral("Corybas_Corysanthes diemenica"));
}

void TestFilenameParser::uncertaintyMarkersAreLeftInName()
{
    QCOMPARE(parseStem(QStringLiteral("Dianella revoluta s.l - Stawell 1-1-2020")).name,
             QStringLiteral("Dianella revoluta s.l"));
    QCOMPARE(parseStem(QStringLiteral("Caladenia sp aff concolor - Brisbane Ranges 9-9-2019")).name,
             QStringLiteral("Caladenia sp aff concolor"));
}

void TestFilenameParser::baseNameStripsOnlyFinalExtension()
{
    QCOMPARE(captureBaseName(QStringLiteral("/x/y/Diuris pardina s.l - Loc 1-2-2020 (1).JPG")),
             QStringLiteral("Diuris pardina s.l - Loc 1-2-2020 (1)"));
    QCOMPARE(captureBaseName(QStringLiteral("Cycnogeton procerum s.s.NEF")),
             QStringLiteral("Cycnogeton procerum s.s"));
}

void TestFilenameParser::emptyStem()
{
    const ParsedFilename p = parseStem(QString());
    QVERIFY(p.name.isEmpty());
    QVERIFY(p == ParsedFilename{});
}

QTEST_APPLESS_MAIN(TestFilenameParser)
#include "tst_filenameparser.moc"
