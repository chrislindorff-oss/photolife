#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TreeGapFiller.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

// A tree whose species names a level (a complex) the tree hasn't got: it
// hangs from its nearest ancestor meanwhile, and the gap filler fetches it.
class TestTreeGapFiller : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void parentWithinFallsBackToTheNearestAncestorHere();
    void treeHangsAnOrphanFromItsNearestAncestor();
    void listsOnlyGapsBelowTheTree();
    void fetchesAndLinksTheMissingLevel();
    void linksAKnownLevelWithoutAsking();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    QHash<qint64, QJsonObject> m_now;
    int m_project = 0;
    int m_calls = 0;

    void add(qint64 id, const char *rank, const char *name, qint64 parent, const char *ancestry, bool link = true)
    {
        Taxon t;
        t.inatId = id;
        t.rank = QLatin1String(rank);
        t.name = QLatin1String(name);
        if (parent)
            t.parentInatId = parent;
        t.ancestry = QLatin1String(ancestry);
        QVERIFY(m_store->upsertTaxon(t) > 0);
        if (link)
            QVERIFY(m_store->addProjectTaxon(m_project, id, true, false));
    }
    std::optional<qint64> parentInTree(qint64 id) const
    {
        for (const TreeNode &n : m_store->projectTree(m_project))
            if (n.inatId == id)
                return n.parentInatId;
        return std::nullopt;
    }
    int fill()
    {
        TreeGapFiller filler(*m_inat, *m_store);
        QSignalSpy spy(&filler, &TreeGapFiller::finished);
        filler.fill(m_project);
        if (spy.isEmpty() && !spy.wait(5000))
            return -1;
        return spy.at(0).at(1).toInt();
    }
};

void TestTreeGapFiller::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
    m_calls = 0;
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [this](const Transport::Request &req, int) {
        ++m_calls;
        QJsonArray results;
        for (const QString &id : req.url.path().section(QLatin1Char('/'), -1).split(QLatin1Char(',')))
            if (m_now.contains(id.toLongLong()))
                results.append(m_now.value(id.toLongLong()));
        return FakeTransport::ok(QJsonDocument(QJsonObject{{QStringLiteral("results"), results}}).toJson());
    };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));

    // Araliaceae (the tree's top, under a kingdom it doesn't hold) > Hedera >
    // Hedera hibernica, and Hedera helix -- whose record now names the
    // "Hedera helix" complex (150), which the tree hasn't got.
    Taxon top;
    top.inatId = 10;
    top.rank = QStringLiteral("family");
    top.name = QStringLiteral("Araliaceae");
    top.parentInatId = 1;
    top.ancestry = QStringLiteral("1");
    QVERIFY(m_store->upsertTaxon(top) > 0);
    m_project = m_store->ensureProject(QStringLiteral("Ivies"), 10, std::nullopt, QStringLiteral("inat"));
    QVERIFY(m_store->addProjectTaxon(m_project, 10, false, false));
    add(20, "genus", "Hedera", 10, "1/10");
    add(101, "species", "Hedera hibernica", 20, "1/10/20");
    add(102, "species", "Hedera helix", 150, "1/10/20/150");
    m_now.insert(150, QJsonObject{{QStringLiteral("id"), 150}, {QStringLiteral("rank"), QStringLiteral("complex")},
                                  {QStringLiteral("name"), QStringLiteral("Hedera helix")},
                                  {QStringLiteral("parent_id"), 20},
                                  {QStringLiteral("ancestry"), QStringLiteral("1/10/20")},
                                  {QStringLiteral("is_active"), true}});
}

void TestTreeGapFiller::cleanup()
{
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
    m_now.clear();
}

void TestTreeGapFiller::parentWithinFallsBackToTheNearestAncestorHere()
{
    const QSet<qint64> present{10, 20};
    QCOMPARE(TaxonomyStore::parentWithin(20, QStringLiteral("1/10/20"), present), std::optional<qint64>(20));
    QCOMPARE(TaxonomyStore::parentWithin(150, QStringLiteral("1/10/20/150"), present), std::optional<qint64>(20));
    // Nothing above it here: unchanged (it's the tree's top).
    QCOMPARE(TaxonomyStore::parentWithin(1, QStringLiteral("1"), present), std::optional<qint64>(1));
    QCOMPARE(TaxonomyStore::parentWithin(std::nullopt, QString(), present), std::optional<qint64>());
}

void TestTreeGapFiller::treeHangsAnOrphanFromItsNearestAncestor()
{
    QCOMPARE(parentInTree(102), std::optional<qint64>(20));   // under Hedera, not alone at the top
    QCOMPARE(parentInTree(101), std::optional<qint64>(20));
    QCOMPARE(parentInTree(10), std::optional<qint64>(1));     // the top keeps its outside parent
}

void TestTreeGapFiller::listsOnlyGapsBelowTheTree()
{
    // The complex -- not the kingdom above the tree's top.
    QCOMPARE(m_store->projectMissingAncestorIds(m_project), QList<qint64>{150});
}

void TestTreeGapFiller::fetchesAndLinksTheMissingLevel()
{
    QCOMPARE(fill(), 1);
    QCOMPARE(parentInTree(150), std::optional<qint64>(20));
    QCOMPARE(parentInTree(102), std::optional<qint64>(150));
    QVERIFY(m_store->projectMissingAncestorIds(m_project).isEmpty());
    // Nothing left to do: no more asking.
    const int asked = m_calls;
    QCOMPARE(fill(), 0);
    QCOMPARE(m_calls, asked);
}

void TestTreeGapFiller::linksAKnownLevelWithoutAsking()
{
    // Another tree has the complex already.
    add(150, "complex", "Hedera helix", 20, "1/10/20", false);
    QCOMPARE(fill(), 1);
    QCOMPARE(m_calls, 0);
    QCOMPARE(parentInTree(102), std::optional<qint64>(150));
}

QTEST_GUILESS_MAIN(TestTreeGapFiller)
#include "tst_treegapfiller.moc"
