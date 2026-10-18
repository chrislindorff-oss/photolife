#include <QtTest>

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"
#include "treefile/TreeImporter.h"

using namespace pl;
using namespace pl::net;
using namespace pl::treefile;

namespace {

Transport::Reply route(const Transport::Request &req)
{
    const QString path = req.url.path();
    const QString q = QUrlQuery(req.url).queryItemValue(QStringLiteral("q"));

    if (path.endsWith(QLatin1String("/taxa")) && !q.isEmpty()) {
        if (q == QLatin1String("Diuris pardina"))
            return FakeTransport::ok(R"({"results":[
                {"id":899,"rank":"species","rank_level":10,"name":"Diuris pardinella","is_active":true,"ancestry":"48460/47217/100"},
                {"id":900,"rank":"species","rank_level":10,"name":"Diuris pardina","is_active":true,"ancestry":"48460/47217/100"}]})");
        if (q == QLatin1String("Diuris sulphurea"))
            return FakeTransport::ok(R"({"results":[{"id":901,"rank":"species","rank_level":10,"name":"Diuris sulphurea","is_active":true,"ancestry":"48460/47217/100"}]})");
        return FakeTransport::ok(R"({"results":[]})");
    }
    if (path.contains(QLatin1String("/taxa/"))) {   // by id: ancestors, excluded taxa
        return FakeTransport::ok(R"({"results":[
            {"id":48460,"rank":"stateofmatter","rank_level":100,"name":"Life"},
            {"id":47217,"rank":"family","rank_level":30,"name":"Orchidaceae","ancestry":"48460"},
            {"id":100,"rank":"genus","rank_level":20,"name":"Diuris","ancestry":"48460/47217"},
            {"id":5001,"rank":"genus","rank_level":20,"name":"Caladenia","ancestry":"48460/47217"}]})");
    }
    return FakeTransport::httpStatus(404);
}

TreeFileRow row(qint64 id, std::optional<qint64> parent, const char *rank, int level,
                const char *name, const char *ancestry, bool inRegion = false)
{
    TreeFileRow r;
    r.inatId = id;
    r.parentInatId = parent;
    r.rank = QLatin1String(rank);
    r.rankLevel = level;
    r.name = QLatin1String(name);
    r.ancestry = QLatin1String(ancestry);
    r.inRegion = inRegion;
    return r;
}

// What exportProject() writes for a small tree: every rank, ids throughout.
TreeFile exportedFile()
{
    TreeFile f;
    f.formatVersion = TreeFile::kFormatVersion;
    f.name = QStringLiteral("Orchids of Victoria");
    f.rootInatId = 47217;
    f.placeInatId = 7830;
    f.rows = {row(48460, std::nullopt, "stateofmatter", 100, "Life", ""),
              row(47217, 48460, "family", 30, "Orchidaceae", "48460"),
              row(100, 47217, "genus", 20, "Diuris", "48460/47217"),
              row(900, 100, "species", 10, "Diuris pardina", "48460/47217/100", true)};
    f.rows[3].fromChecklist = true;
    f.rows[3].commonName = QStringLiteral("Leopard Orchid");
    f.rows[3].statuses = {{QStringLiteral("Rare"), QStringLiteral("VBA 2021"), 7830}};
    f.rows[3].origins = {{QStringLiteral("native"), QString(), std::nullopt}};
    return f;
}

TreeFileRow nameOnly(const char *name)
{
    TreeFileRow r;
    r.name = QLatin1String(name);
    return r;
}

} // namespace

class TestTreeImporter : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void exportedFileImportsWithoutNetwork();
    void resolvesNameOnlyRowsAndFillsAncestors();
    void exportedFileKeepsItsOwnShape();
    void fillsAncestorsOfHandMadeIdRows();
    void restoresExclusions();
    void keepsCachedTaxonNames();
    void refusesExistingTreeName();
    void cancelRemovesPartialTree();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<taxonomy::TaxonomyStore> m_store;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    std::unique_ptr<TreeImporter> m_importer;

    int scalar(const QString &sql);
    QList<QVariant> run(const TreeFile &file, const QString &name = QStringLiteral("Imported"));
};

void TestTreeImporter::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<taxonomy::TaxonomyStore>(m_db->connectionName());

    taxonomy::Place victoria;
    victoria.inatId = 7830;
    victoria.name = QStringLiteral("Victoria");
    m_store->upsertPlace(victoria);

    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [](const Transport::Request &r, int) { return route(r); };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_http->setRetryBaseDelayMs(1);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));
    m_importer = std::make_unique<TreeImporter>(*m_inat, *m_store);
}

void TestTreeImporter::cleanup()
{
    m_importer.reset();
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
}

int TestTreeImporter::scalar(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.exec(sql);
    return q.next() ? q.value(0).toInt() : -1;
}

// finished(ok, error, projectId, imported, lookedUp, unresolved)
QList<QVariant> TestTreeImporter::run(const TreeFile &file, const QString &name)
{
    QSignalSpy spy(m_importer.get(), &TreeImporter::finished);
    m_importer->start({name, file, QStringLiteral("tree file")});
    if (spy.isEmpty())
        spy.wait(5000);
    return spy.isEmpty() ? QList<QVariant>() : spy.at(0);
}

void TestTreeImporter::exportedFileImportsWithoutNetwork()
{
    const auto result = run(exportedFile());
    QCOMPARE(result.value(0).toBool(), true);
    QCOMPARE(result.value(3).toInt(), 4);
    QCOMPARE(result.value(4).toInt(), 0);
    QCOMPARE(result.value(5).toInt(), 0);
    QCOMPARE(m_transport->received.size(), 0);

    const int pid = result.value(2).toInt();
    QCOMPARE(m_store->projectRootTaxonInatId(pid), std::optional<qint64>(47217));
    QCOMPARE(m_store->projectPlaceInatId(pid), std::optional<qint64>(7830));
    const auto tree = m_store->projectTree(pid);
    QCOMPARE(tree.size(), 4);
    for (const auto &n : tree) {
        QCOMPARE(n.inRegion, n.inatId == 900);
        QCOMPARE(n.fromChecklist, n.inatId == 900);
    }

    const auto statuses = m_store->projectStatuses(pid);
    QCOMPARE(statuses.size(), 1);
    QCOMPARE(statuses.at(0).source, QStringLiteral("VBA 2021"));
    const auto origins = m_store->projectOrigins(pid);
    QCOMPARE(origins.size(), 1);
    // No source or place in the file: the request's default and the tree's place.
    QCOMPARE(origins.at(0).source, QStringLiteral("tree file"));
    QCOMPARE(origins.at(0).placeInatId, std::optional<qint64>(7830));

    // The file's common name, for matching photos filed under it.
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
                                   "WHERE t.inat_id = 900 AND tn.name = 'Leopard Orchid' "
                                   "AND tn.kind = 'user-vernacular'")), 1);
}

void TestTreeImporter::resolvesNameOnlyRowsAndFillsAncestors()
{
    TreeFile file;
    file.rows = {nameOnly("Diuris pardina"), nameOnly("Diuris sulphurea"),
                 nameOnly("Diuris ghostus")};
    file.rows[0].statuses = {{QStringLiteral("Rare"), QString(), std::nullopt}};

    const auto result = run(file);
    QCOMPARE(result.value(0).toBool(), true);
    QCOMPARE(result.value(3).toInt(), 2);   // imported
    QCOMPARE(result.value(4).toInt(), 2);   // both looked up
    QCOMPARE(result.value(5).toInt(), 1);   // Diuris ghostus

    const int pid = result.value(2).toInt();
    QSet<qint64> ids;
    for (const auto &n : m_store->projectTree(pid))
        ids.insert(n.inatId);
    // The exact-name match (900), not the first result (899), plus ancestors.
    QCOMPARE(ids, (QSet<qint64>{48460, 47217, 100, 900, 901}));
    // No root in the file: the deepest taxon both species share.
    QCOMPARE(m_store->projectRootTaxonInatId(pid), std::optional<qint64>(100));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM project_taxon WHERE project_id = %1 "
                                   "AND from_checklist = 1").arg(pid)), 2);
    QCOMPARE(m_store->projectStatuses(pid).value(0).source, QStringLiteral("tree file"));
}

// An exported tree with a gap (a taxon whose parent the tree doesn't hold)
// comes back with the same gap -- not filled from the network.
void TestTreeImporter::exportedFileKeepsItsOwnShape()
{
    TreeFile file = exportedFile();
    file.rows.removeFirst();   // Life, the family's parent
    const auto result = run(file);
    QCOMPARE(result.value(0).toBool(), true);
    QCOMPARE(m_transport->received.size(), 0);
    QCOMPARE(m_store->projectTree(result.value(2).toInt()).size(), 3);
}

void TestTreeImporter::fillsAncestorsOfHandMadeIdRows()
{
    TreeFile file = exportedFile();
    file.formatVersion = 0;
    file.rows.removeFirst();
    const auto result = run(file);
    QCOMPARE(result.value(0).toBool(), true);
    QCOMPARE(m_transport->received.size(), 1);   // Life, fetched by id
    QCOMPARE(m_store->projectTree(result.value(2).toInt()).size(), 4);
}

void TestTreeImporter::restoresExclusions()
{
    TreeFile file = exportedFile();
    file.excludedInatIds = {5001};   // pruned, so not cached here: fetched by id
    const auto result = run(file);
    QCOMPARE(result.value(0).toBool(), true);
    const int pid = result.value(2).toInt();
    QCOMPARE(m_store->projectExcludedTaxonIds(pid), QList<qint64>{5001});
    QCOMPARE(m_store->projectTree(pid).size(), 4);   // excluded, not linked
}

void TestTreeImporter::keepsCachedTaxonNames()
{
    taxonomy::Taxon cached;
    cached.inatId = 900;
    cached.rank = QStringLiteral("species");
    cached.name = QStringLiteral("Diuris pardina");
    cached.ancestry = QStringLiteral("48460/47217/100");
    cached.synonyms = {QStringLiteral("Diuris maculata")};
    m_store->upsertTaxon(cached);

    TreeFile file = exportedFile();
    file.rows[3].name = QStringLiteral("Diuris edited");
    QCOMPARE(run(file).value(0).toBool(), true);
    QCOMPARE(m_store->taxonPhoto(900).name, QStringLiteral("Diuris pardina"));
    QVERIFY(m_store->taxonInatIdByFoldedName(QStringLiteral("diuris maculata")));
}

void TestTreeImporter::refusesExistingTreeName()
{
    m_store->ensureProject(QStringLiteral("Taken"), std::nullopt, std::nullopt, QString());
    const auto result = run(exportedFile(), QStringLiteral("Taken"));
    QCOMPARE(result.value(0).toBool(), false);
    QVERIFY(result.value(1).toString().contains(QStringLiteral("already exists")));
    QVERIFY(m_store->projectIdByName(QStringLiteral("Taken")));   // left alone
}

void TestTreeImporter::cancelRemovesPartialTree()
{
    TreeFile file;
    file.rows = {nameOnly("Diuris pardina")};
    QSignalSpy spy(m_importer.get(), &TreeImporter::finished);
    m_importer->start({QStringLiteral("Doomed"), file, QString()});
    m_importer->cancel();   // while the search is in flight
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toBool(), false);
    QVERIFY(!m_store->projectIdByName(QStringLiteral("Doomed")));
}

QTEST_GUILESS_MAIN(TestTreeImporter)
#include "tst_treeimporter.moc"
