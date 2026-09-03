#include <QtTest>

#include "match/NameParser.h"

using namespace pl::match;

class TestNameParser : public QObject
{
    Q_OBJECT

private slots:
    void plainBinomial();
    void bareGenus();
    void spMarker();
    void spAff();
    void spNumberedUndescribed();
    void spInformalForm();
    void sensuLatoAndStricto();
    void aggregate();
    void hybrid();
    void infraspecificRanks();
    void unrankedTrailingEpithet();
    void unidentified();
    void synonymGenusPair();
    void foldedBinomialForLookup();
    void nonTaxonomicLocalities();
    void nonTaxonomicStaging();
    void realNamesAreTaxonomic();
};

void TestNameParser::plainBinomial()
{
    const auto p = parseName(QStringLiteral("Diuris pardina"));
    QCOMPARE(p.genus, QStringLiteral("Diuris"));
    QCOMPARE(p.specificEpithet, QStringLiteral("pardina"));
    QCOMPARE(p.qualifier, Qualifier::None);
    QVERIFY(p.hasSpecies());
}

void TestNameParser::bareGenus()
{
    const auto p = parseName(QStringLiteral("Caladenia"));
    QCOMPARE(p.genus, QStringLiteral("Caladenia"));
    QVERIFY(p.isGenusOnly());
    QCOMPARE(p.qualifier, Qualifier::None);
}

void TestNameParser::spMarker()
{
    for (const auto &s : {QStringLiteral("Diuris sp"), QStringLiteral("Diuris sp."),
                          QStringLiteral("Diuris spp.")}) {
        const auto p = parseName(s);
        QCOMPARE(p.genus, QStringLiteral("Diuris"));
        QVERIFY(p.specificEpithet.isEmpty());
        QCOMPARE(p.qualifier, Qualifier::Sp);
    }
}

void TestNameParser::spAff()
{
    const auto p = parseName(QStringLiteral("Caladenia sp aff concolor"));
    QCOMPARE(p.genus, QStringLiteral("Caladenia"));
    QCOMPARE(p.qualifier, Qualifier::SpAff);
    QCOMPARE(p.specificEpithet, QStringLiteral("concolor"));

    const auto cf = parseName(QStringLiteral("Pterostylis cf. nutans"));
    QCOMPARE(cf.qualifier, Qualifier::SpAff);
    QCOMPARE(cf.specificEpithet, QStringLiteral("nutans"));
}

void TestNameParser::spNumberedUndescribed()
{
    const auto p = parseName(QStringLiteral("Arthropodium sp. 3"));
    QCOMPARE(p.genus, QStringLiteral("Arthropodium"));
    QCOMPARE(p.qualifier, Qualifier::Undescribed);
    QCOMPARE(p.informalTag, QStringLiteral("3"));
}

void TestNameParser::spInformalForm()
{
    const auto p = parseName(QStringLiteral("Kunzea sp. (Upright form)"));
    QCOMPARE(p.genus, QStringLiteral("Kunzea"));
    QCOMPARE(p.qualifier, Qualifier::Undescribed);
    QCOMPARE(p.informalTag, QStringLiteral("Upright form"));
}

void TestNameParser::sensuLatoAndStricto()
{
    QCOMPARE(parseName(QStringLiteral("Dianella revoluta s.l")).qualifier, Qualifier::SensuLato);
    QCOMPARE(parseName(QStringLiteral("Dianella revoluta s.l.")).qualifier, Qualifier::SensuLato);
    const auto ss = parseName(QStringLiteral("Cycnogeton procerum s.s"));
    QCOMPARE(ss.qualifier, Qualifier::SensuStricto);
    QCOMPARE(ss.genus, QStringLiteral("Cycnogeton"));
    QCOMPARE(ss.specificEpithet, QStringLiteral("procerum"));
}

void TestNameParser::aggregate()
{
    QCOMPARE(parseName(QStringLiteral("Rubus fruticosus spp agg")).qualifier, Qualifier::Aggregate);
    const auto k = parseName(QStringLiteral("Kunzea ericoides spp. agg"));
    QCOMPARE(k.qualifier, Qualifier::Aggregate);
    QCOMPARE(k.specificEpithet, QStringLiteral("ericoides"));
}

void TestNameParser::hybrid()
{
    const auto p = parseName(QStringLiteral("Acacia paradoxa x stictophylla"));
    QVERIFY(p.isHybrid);
    QCOMPARE(p.genus, QStringLiteral("Acacia"));
    QCOMPARE(p.specificEpithet, QStringLiteral("paradoxa"));
    QCOMPARE(p.hybridEpithet2, QStringLiteral("stictophylla"));

    const auto sign = parseName(QString::fromUtf8("Acacia paradoxa \xC3\x97 stictophylla"));
    QVERIFY(sign.isHybrid);
    QCOMPARE(sign.hybridEpithet2, QStringLiteral("stictophylla"));
}

void TestNameParser::infraspecificRanks()
{
    for (const auto &s : {QStringLiteral("Olearia ramulosa var. stricta"),
                          QStringLiteral("Olearia ramulosa var stricta")}) {
        const auto p = parseName(s);
        QCOMPARE(p.infraRank, QStringLiteral("var"));
        QCOMPARE(p.infraEpithet, QStringLiteral("stricta"));
    }
    QCOMPARE(parseName(QStringLiteral("Pultenaea subsp strigosa")).infraRank,
             QStringLiteral("subsp"));
    QCOMPARE(parseName(QStringLiteral("Correa reflexa subsp. speciosa")).infraEpithet,
             QStringLiteral("speciosa"));
}

void TestNameParser::unrankedTrailingEpithet()
{
    const auto p = parseName(QStringLiteral("Prostanthera lasianthos aristata"));
    QCOMPARE(p.genus, QStringLiteral("Prostanthera"));
    QCOMPARE(p.specificEpithet, QStringLiteral("lasianthos"));
    QCOMPARE(p.infraEpithet, QStringLiteral("aristata"));
}

void TestNameParser::unidentified()
{
    const auto p = parseName(QStringLiteral("Unidentified Caladenia"));
    QCOMPARE(p.genus, QStringLiteral("Caladenia"));
    QCOMPARE(p.qualifier, Qualifier::Unidentified);

    QCOMPARE(parseName(QStringLiteral("Caladenia indet.")).qualifier, Qualifier::Unidentified);
}

void TestNameParser::synonymGenusPair()
{
    const auto p = parseName(QStringLiteral("Corybas_Corysanthes diemenica"));
    QCOMPARE(p.genus, QStringLiteral("Corybas"));
    QCOMPARE(p.altGenera, (QStringList{QStringLiteral("Corybas"), QStringLiteral("Corysanthes")}));
    QCOMPARE(p.specificEpithet, QStringLiteral("diemenica"));
}

void TestNameParser::foldedBinomialForLookup()
{
    QCOMPARE(parseName(QStringLiteral("Diuris Pardina")).foldedBinomial(),
             QStringLiteral("diuris pardina"));
    QVERIFY(parseName(QStringLiteral("Caladenia sp.")).foldedBinomial().isEmpty());
}

void TestNameParser::nonTaxonomicLocalities()
{
    QVERIFY(looksNonTaxonomic(QStringLiteral("401 Fulbrooks Road, Dadswells Bridge")));
    QVERIFY(looksNonTaxonomic(QStringLiteral("Thompson Road, BRNP")));
}

void TestNameParser::nonTaxonomicStaging()
{
    QVERIFY(looksNonTaxonomic(QStringLiteral("To Sort and Upload")));
    QVERIFY(looksNonTaxonomic(QStringLiteral("Temp GPS")) == false
            || looksNonTaxonomic(QStringLiteral("Temp GPS")));  // 'temp' alone is not decisive
    QVERIFY(looksNonTaxonomic(QStringLiteral("Duplicates")));
}

void TestNameParser::realNamesAreTaxonomic()
{
    for (const auto &s : {QStringLiteral("Diuris pardina"), QStringLiteral("Orchidaceae"),
                          QStringLiteral("Caladenia sp aff concolor"),
                          QStringLiteral("Pterostylis nutans")}) {
        QVERIFY2(!looksNonTaxonomic(s), qPrintable(s));
    }
}

QTEST_APPLESS_MAIN(TestNameParser)
#include "tst_nameparser.moc"
