#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>

#include "db/Database.h"
#include "match/NameParser.h"
#include "match/TaxonResolver.h"
#include "match/TextSimilarity.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::match;

class TestTaxonResolver : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void similarityBasics();
    void exactAcceptedName();
    void synonymResolvesToAcceptedTaxon();
    void folderGenusDisagreementIsNotPenalisedForSynonyms();
    void genusOnlyResolvesToGenus();
    void fuzzyMatchesTypo();
    void aliasBeatsEverything();
    void resolvesCommonNameFromRawString();
    void commonNames();
    void nonTaxonMarker();
    void nothingMatches();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<taxonomy::TaxonomyStore> m_store;
    std::unique_ptr<TaxonResolver> m_resolver;

    void seed();
};

void TestTaxonResolver::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<taxonomy::TaxonomyStore>(m_db->connectionName());
    m_resolver = std::make_unique<TaxonResolver>(m_db->connectionName());
    seed();
}

void TestTaxonResolver::cleanup()
{
    m_resolver.reset();
    m_store.reset();
    m_db.reset();
}

void TestTaxonResolver::seed()
{
    taxonomy::Taxon caladenia;
    caladenia.inatId = 100;
    caladenia.rank = QStringLiteral("genus");
    caladenia.name = QStringLiteral("Caladenia");
    m_store->upsertTaxon(caladenia);

    taxonomy::Taxon carnea;
    carnea.inatId = 101;
    carnea.parentInatId = 100;
    carnea.rank = QStringLiteral("species");
    carnea.name = QStringLiteral("Caladenia carnea");
    carnea.commonName = QStringLiteral("Pink Fingers");
    carnea.synonyms = {QStringLiteral("Petalochilus carneus")};
    m_store->upsertTaxon(carnea);

    taxonomy::Taxon asparagus;
    asparagus.inatId = 200;
    asparagus.rank = QStringLiteral("species");
    asparagus.name = QStringLiteral("Asparagus scandens");
    m_store->upsertTaxon(asparagus);
}

void TestTaxonResolver::similarityBasics()
{
    QCOMPARE(editDistance(QStringLiteral("asaparagus"), QStringLiteral("asparagus")), 1);
    QVERIFY(nameSimilarity(QStringLiteral("asaparagus scandens"),
                           QStringLiteral("asparagus scandens")) > 0.9);
    QVERIFY(nameSimilarity(QStringLiteral("diuris pardina"),
                           QStringLiteral("caladenia carnea")) < 0.4);
}

void TestTaxonResolver::exactAcceptedName()
{
    const auto c = m_resolver->resolve(parseName(QStringLiteral("Caladenia carnea")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Caladenia carnea"));
    QCOMPARE(c.first().matchedVia, QStringLiteral("accepted"));
    QCOMPARE(c.first().score, 1.0);
}

void TestTaxonResolver::synonymResolvesToAcceptedTaxon()
{
    const auto c = m_resolver->resolve(parseName(QStringLiteral("Petalochilus carneus")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Caladenia carnea"));
    QCOMPARE(c.first().matchedVia, QStringLiteral("synonym"));
    QVERIFY(c.first().score > 0.9);
}

void TestTaxonResolver::folderGenusDisagreementIsNotPenalisedForSynonyms()
{
    ResolveHints hints;
    hints.genus = QStringLiteral("Caladenia");   // folder says Caladenia, file says Petalochilus
    const auto c = m_resolver->resolve(parseName(QStringLiteral("Petalochilus carneus")), hints);
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Caladenia carnea"));
    QVERIFY(c.first().score > 0.9);   // synonym match, no penalty
}

void TestTaxonResolver::genusOnlyResolvesToGenus()
{
    const auto c = m_resolver->resolve(parseName(QStringLiteral("Caladenia sp.")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Caladenia"));
    QCOMPARE(c.first().rank, QStringLiteral("genus"));
}

void TestTaxonResolver::fuzzyMatchesTypo()
{
    const auto c = m_resolver->resolve(parseName(QStringLiteral("Asaparagus scandens")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Asparagus scandens"));
    QCOMPARE(c.first().matchedVia, QStringLiteral("fuzzy"));
    QVERIFY(c.first().score > 0.5);
    QVERIFY(c.first().score < 0.75);   // fuzzy is capped
}

void TestTaxonResolver::aliasBeatsEverything()
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "INSERT INTO name_alias (raw_text, raw_folded, taxon_id, scope) "
        "VALUES (?, ?, (SELECT id FROM taxon WHERE inat_id = 101), 'global')"));
    q.addBindValue(QStringLiteral("weird old name"));
    q.addBindValue(taxonomy::TaxonomyStore::foldName(QStringLiteral("weird old name")));
    QVERIFY(q.exec());

    const auto c = m_resolver->resolve(parseName(QStringLiteral("weird old name")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().matchedVia, QStringLiteral("alias"));
    QCOMPARE(c.first().name, QStringLiteral("Caladenia carnea"));
}

void TestTaxonResolver::resolvesCommonNameFromRawString()
{
    taxonomy::Taxon duck;
    duck.inatId = 7000;
    duck.rank = QStringLiteral("species");
    duck.name = QStringLiteral("Anas superciliosa");
    duck.vernacular = {QStringLiteral("Pacific Black Duck")};
    m_store->upsertTaxon(duck);

    const auto c = m_resolver->resolve(parseName(QStringLiteral("Pacific Black Duck")));
    QVERIFY(!c.isEmpty());
    QCOMPARE(c.first().name, QStringLiteral("Anas superciliosa"));
    QCOMPARE(c.first().matchedVia, QStringLiteral("vernacular"));
}

// Common names: one that means a single species is as good as its
// scientific name; a looser spelling nearly so; a group's name, or one
// shared by several taxa, goes to review.
void TestTaxonResolver::commonNames()
{
    auto add = [&](qint64 id, std::optional<qint64> parent, const char *rank, const char *name,
                   QStringList common) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.parentInatId = parent;
        t.rank = QLatin1String(rank);
        t.name = QString::fromUtf8(name);
        t.commonName = common.value(0);
        t.vernacular = common;
        t.allNames = true;
        QVERIFY(m_store->upsertTaxon(t) > 0);
    };
    add(300, std::nullopt, "species", "Petroica phoenicea", {QStringLiteral("Flame Robin")});
    add(310, std::nullopt, "species", "Malurus cyaneus", {QStringLiteral("Superb Fairywren")});
    add(311, 310, "subspecies", "Malurus cyaneus cyanochlamys", {QStringLiteral("Superb Fairywren")});
    add(320, std::nullopt, "species", "Gallinago hardwickii", {QStringLiteral("Latham's Snipe")});
    add(330, std::nullopt, "genus", "Thelymitra", {QStringLiteral("Sun Orchids")});
    add(340, std::nullopt, "species", "Heteromyias cinereifrons", {QStringLiteral("Grey Robin")});
    add(341, std::nullopt, "species", "Peneothello cyanus", {QStringLiteral("Grey Robin")});
    add(350, std::nullopt, "species", "Porphyrio melanotus",
        {QStringLiteral("Australasian Swamphen"), QStringLiteral("Purple Swamphen")});

    TaxonResolver resolver(m_db->connectionName());
    auto top = [&](const char *text) {
        const auto c = resolver.resolve(parseName(QString::fromUtf8(text)));
        return c.isEmpty() ? TaxonCandidate{} : c.first();
    };

    QCOMPARE(top("Flame Robin").inatId, qint64(300));
    QCOMPARE(top("Flame Robin").score, TaxonResolver::kUniqueCommonName);
    QCOMPARE(top("flame robin").inatId, qint64(300));
    QCOMPARE(top("Purple Swamphen").inatId, qint64(350));   // not the preferred name
    QCOMPARE(top("Purple Swamphen").score, TaxonResolver::kUniqueCommonName);

    // Hyphens, spaces and apostrophes don't matter.
    QCOMPARE(top("Superb Fairy-wren").inatId, qint64(310));   // the species, not its subspecies
    QCOMPARE(top("Superb Fairy-wren").score, TaxonResolver::kUniqueLooseCommonName);
    QCOMPARE(top("Lathams Snipe").inatId, qint64(320));
    QCOMPARE(top("Lathams Snipe").score, TaxonResolver::kUniqueLooseCommonName);

    // Inverted, and paired with its scientific name.
    QCOMPARE(top("Robin, Flame").inatId, qint64(300));
    QCOMPARE(top("Flame Robin · Petroica phoenicea").inatId, qint64(300));
    QCOMPARE(top("Flame Robin · Petroica phoenicea").score, 1.0);
    QCOMPARE(top("Flame Robin (Petroica phoenicea)").inatId, qint64(300));
    QVERIFY(top("Flame Robin · Peneothello cyanus").score < 0.9);   // they disagree

    // A group's name, or a shared one: for review.
    QCOMPARE(top("Sun Orchids").inatId, qint64(330));
    QCOMPARE(top("Sun Orchids").score, TaxonResolver::kOtherCommonName);
    const auto grey = resolver.resolve(parseName(QStringLiteral("Grey Robin")));
    QCOMPARE(grey.size(), 2);
    QCOMPARE(grey.first().score, TaxonResolver::kOtherCommonName);

    // Scientific names are untouched.
    QCOMPARE(top("Caladenia carnea").inatId, qint64(101));
    QCOMPARE(top("Caladenia carnea").score, 1.0);
}

void TestTaxonResolver::nonTaxonMarker()
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "INSERT INTO name_alias (raw_text, raw_folded, not_a_taxon, scope) VALUES (?, ?, 1, ?)"));
    q.addBindValue(QStringLiteral("Thompson Track"));
    q.addBindValue(taxonomy::TaxonomyStore::foldName(QStringLiteral("Thompson Track")));
    q.addBindValue(QStringLiteral("/lib/Diuris/Diuris pardina"));
    QVERIFY(q.exec());

    QVERIFY(m_resolver->isMarkedNonTaxon(QStringLiteral("Thompson Track"),
                                         QStringLiteral("/lib/Diuris/Diuris pardina")));
    QVERIFY(!m_resolver->isMarkedNonTaxon(QStringLiteral("Thompson Track"),
                                          QStringLiteral("/other/path")));
}

void TestTaxonResolver::nothingMatches()
{
    QVERIFY(m_resolver->resolve(parseName(QStringLiteral("Zzyzx nonexistus"))).isEmpty());
}

QTEST_GUILESS_MAIN(TestTaxonResolver)
#include "tst_taxonresolver.moc"
