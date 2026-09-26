#include <QtTest>

#include "checklist/ChecklistParser.h"

using namespace pl::checklist;

class TestChecklistParser : public QObject
{
    Q_OBJECT

private slots:
    void parsesNameStatusPairs();
    void stripsBomAndHandlesCrlf();
    void emptyStatusIsKept();
    void skipsBlankLinesAndEmptyNames();
    void skipsHeaderRow();
    void handlesQuotedFieldsWithCommas();
    void ignoresExtraColumns();
    void realVbaSample();
    void readsOriginFromHeaderColumn();
    void readsOriginFromThirdColumnValues();
    void starBeforeNameMeansIntroduced();
};

void TestChecklistParser::parsesNameStatusPairs()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "Abrotanella nivigena,Vulnerable\nCaladenia carnea,\n"));
    QCOMPARE(e.size(), 2);
    QCOMPARE(e.at(0).name, QStringLiteral("Abrotanella nivigena"));
    QCOMPARE(e.at(0).status, QStringLiteral("Vulnerable"));
    QCOMPARE(e.at(0).statusFolded, QStringLiteral("vulnerable"));
}

void TestChecklistParser::stripsBomAndHandlesCrlf()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "\xEF\xBB\xBF" "Abelia X grandiflora,\r\nAbrodictyum caudatum,Rare\r\n"));
    QCOMPARE(e.size(), 2);
    QCOMPARE(e.at(0).name, QStringLiteral("Abelia X grandiflora"));
    QCOMPARE(e.at(1).status, QStringLiteral("Rare"));
}

void TestChecklistParser::emptyStatusIsKept()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral("Abutilon fraseri,\n"));
    QCOMPARE(e.size(), 1);
    QVERIFY(e.at(0).status.isEmpty());
}

void TestChecklistParser::skipsBlankLinesAndEmptyNames()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "Diuris pardina,Rare\n\n  \n,Endangered\nCaladenia carnea,\n"));
    QCOMPARE(e.size(), 2);
}

void TestChecklistParser::skipsHeaderRow()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "Scientific Name,Conservation Status\nDiuris pardina,Rare\n"));
    QCOMPARE(e.size(), 1);
    QCOMPARE(e.at(0).name, QStringLiteral("Diuris pardina"));
}

void TestChecklistParser::handlesQuotedFieldsWithCommas()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "\"Genus species, sensu Jones\",\"Endangered, provisional\"\n"));
    QCOMPARE(e.size(), 1);
    QCOMPARE(e.at(0).name, QStringLiteral("Genus species, sensu Jones"));
    QCOMPARE(e.at(0).status, QStringLiteral("Endangered, provisional"));
}

void TestChecklistParser::ignoresExtraColumns()
{
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "Diuris pardina,Rare,VBA,2021,extra\n"));
    QCOMPARE(e.size(), 1);
    QCOMPARE(e.at(0).status, QStringLiteral("Rare"));
}

void TestChecklistParser::realVbaSample()
{
    // Verbatim from VBA_vascularplants_checklist2021.csv.
    const auto e = parseChecklistCsv(QByteArrayLiteral(
        "\xEF\xBB\xBF" "Abelia X grandiflora,\r\n"
        "Abrodictyum caudatum,Rare\r\n"
        "Abrotanella nivigena,Vulnerable\r\n"
        "Abutilon fraseri,\r\n"
        "Abutilon fraseri subsp. diplotrichum,Endangered\r\n"));
    QCOMPARE(e.size(), 5);
    QCOMPARE(e.at(4).name, QStringLiteral("Abutilon fraseri subsp. diplotrichum"));
    QCOMPARE(e.at(4).status, QStringLiteral("Endangered"));
}

void TestChecklistParser::readsOriginFromHeaderColumn()
{
    // Columns in any order once a header names them.
    const auto entries = parseChecklistCsv(QByteArrayLiteral(
        "Origin,Scientific Name,Conservation Status\n"
        "*,Abutilon grandifolium,\n"
        "Native,Abutilon oxycarpum,Endangered\n"
        ",Abutilon spp.,\n"));
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).name, QStringLiteral("Abutilon grandifolium"));
    QCOMPARE(entries.at(0).origin, QStringLiteral("introduced"));
    QCOMPARE(entries.at(1).status, QStringLiteral("Endangered"));
    QCOMPARE(entries.at(1).origin, QStringLiteral("native"));
    QVERIFY(entries.at(2).origin.isEmpty());
}

void TestChecklistParser::readsOriginFromThirdColumnValues()
{
    const auto entries = parseChecklistCsv(QByteArrayLiteral(
        "Arctotheca calendula,,Naturalised\n"
        "Glossodia major,,native\n"
        "Diuris pardina,Rare,some note\n"));
    QCOMPARE(entries.at(0).origin, QStringLiteral("introduced"));
    QCOMPARE(entries.at(1).origin, QStringLiteral("native"));
    QVERIFY(entries.at(2).origin.isEmpty());   // not an origin value
    QCOMPARE(entries.at(2).status, QStringLiteral("Rare"));
}

void TestChecklistParser::starBeforeNameMeansIntroduced()
{
    const auto entries = parseChecklistCsv(QByteArrayLiteral(
        "*Abutilon grandifolium,\n"
        "* Acacia baileyana,\n"
        "Acacia acinacea,\n"));
    QCOMPARE(entries.at(0).name, QStringLiteral("Abutilon grandifolium"));
    QCOMPARE(entries.at(0).origin, QStringLiteral("introduced"));
    QCOMPARE(entries.at(1).name, QStringLiteral("Acacia baileyana"));
    QCOMPARE(entries.at(1).origin, QStringLiteral("introduced"));
    QVERIFY(entries.at(2).origin.isEmpty());
}

QTEST_APPLESS_MAIN(TestChecklistParser)
#include "tst_checklistparser.moc"
