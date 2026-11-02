#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "FakeTransport.h"
#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TreeChangeFinder.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;

// What iNaturalist says about the tree's taxa now: /taxa/<ids>.
class TestTreeChangeFinder : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void findsWhatIsGoneAndWhy();
    void keptTaxaAreAskedAboutOnlyWhenRenamed();
    void groupsAreEmptyOnlyWhenNothingIsBeneath();
    void movingPhotosToTheNewNameKeepsTheUsersDecisions();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    FakeTransport *m_transport = nullptr;
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<INatClient> m_inat;
    QHash<qint64, QJsonObject> m_now;   // iNaturalist's answer per taxon
    int m_project = 0;

    void taxon(qint64 id, const char *rank, const char *name, qint64 parent, bool inRegion = true,
               bool fromChecklist = false)
    {
        Taxon t;
        t.inatId = id;
        t.rank = QLatin1String(rank);
        t.name = QLatin1String(name);
        if (parent)
            t.parentInatId = parent;
        QVERIFY(m_store->upsertTaxon(t) > 0);
        QVERIFY(m_store->addProjectTaxon(m_project, id, inRegion, fromChecklist));
        now(id, rank, name);
    }
    void now(qint64 id, const char *rank, const char *name, bool active = true, QList<qint64> current = {})
    {
        QJsonArray ids;
        for (qint64 c : current)
            ids.append(c);
        m_now.insert(id, QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("rank"), QLatin1String(rank)},
                                     {QStringLiteral("name"), QLatin1String(name)},
                                     {QStringLiteral("is_active"), active},
                                     {QStringLiteral("current_synonymous_taxon_ids"), ids}});
    }
    QList<TreeChange> find(const QSet<qint64> &listed)
    {
        TreeChangeFinder finder(*m_inat, *m_store);
        QSignalSpy spy(&finder, &TreeChangeFinder::finished);
        finder.find(m_project, listed);
        if (spy.isEmpty() && !spy.wait(5000))
            return {};
        return spy.at(0).at(1).value<QList<TreeChange>>();
    }
    static QStringList describe(const QList<TreeChange> &changes)
    {
        static const char *kinds[] = {"renamed", "split", "retired", "not recorded", "subspecies", "empty"};
        QStringList out;
        for (const TreeChange &c : changes) {
            QStringList to;
            for (const Taxon &r : c.replacements)
                to << r.name;
            out << QStringLiteral("%1: %2%3").arg(QLatin1String(kinds[c.kind]), c.taxon.name,
                                                  to.isEmpty() ? QString() : QStringLiteral(" → ") + to.join(QStringLiteral(", ")));
        }
        return out;
    }
};

void TestTreeChangeFinder::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = [this](const Transport::Request &req, int) {
        const QString ids = req.url.path().section(QLatin1Char('/'), -1);
        QJsonArray results;
        for (const QString &id : ids.split(QLatin1Char(','))) {
            if (m_now.contains(id.toLongLong()))
                results.append(m_now.value(id.toLongLong()));
        }
        return FakeTransport::ok(QJsonDocument(QJsonObject{{QStringLiteral("results"), results}}).toJson());
    };
    m_transport = owned.get();
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_inat = std::make_unique<INatClient>(*m_http);
    m_inat->setBaseUrl(QStringLiteral("https://api.test/v1"));

    // A family's tree: two genera (one now empty) and its species.
    Taxon root;
    root.inatId = 10;
    root.rank = QStringLiteral("family");
    root.name = QStringLiteral("Petroicidae");
    QVERIFY(m_store->upsertTaxon(root) > 0);
    m_project = m_store->ensureProject(QStringLiteral("Robins"), 10, std::nullopt, QStringLiteral("inat"));
    QVERIFY(m_project > 0);
    QVERIFY(m_store->addProjectTaxon(m_project, 10, false, false));
    now(10, "family", "Petroicidae");
    taxon(20, "genus", "Petroica", 10, false);
    taxon(21, "genus", "Emptyia", 10, false);
    taxon(101, "species", "Petroica phoenicea", 20);
    taxon(102, "species", "Petroica oldname", 20);
    taxon(103, "species", "Petroica lumped", 20);
    taxon(104, "species", "Petroica dubia", 20);
    taxon(105, "species", "Petroica vagrans", 20);
    taxon(106, "species", "Petroica listed", 20, true, true);   // from a checklist
    taxon(107, "subspecies", "Petroica phoenicea minor", 101);
    now(102, "species", "Petroica oldname", false, {201});
    now(103, "species", "Petroica lumped", false, {202, 203});
    now(104, "species", "Petroica dubia", false);
    now(201, "species", "Microeca newname");
    now(202, "species", "Petroica alpha");
    now(203, "species", "Petroica beta");
}

void TestTreeChangeFinder::cleanup()
{
    m_inat.reset();
    m_http.reset();
    m_store.reset();
    m_db.reset();
    m_now.clear();
}

// Renamed, split and retired taxa; a species no longer recorded; a
// subspecies with no records of its own; a group left empty. A checklist
// species isn't "not recorded" -- the species list was never where it came
// from.
void TestTreeChangeFinder::findsWhatIsGoneAndWhy()
{
    const QList<TreeChange> changes = find({101});
    QCOMPARE(describe(changes),
             (QStringList{QStringLiteral("renamed: Petroica oldname → Microeca newname"),
                          QStringLiteral("split: Petroica lumped → Petroica alpha, Petroica beta"),
                          QStringLiteral("retired: Petroica dubia"),
                          QStringLiteral("not recorded: Petroica vagrans"),
                          QStringLiteral("subspecies: Petroica phoenicea minor"),
                          QStringLiteral("empty: Emptyia")}));
}

void TestTreeChangeFinder::keptTaxaAreAskedAboutOnlyWhenRenamed()
{
    QVERIFY(m_store->keepProjectTaxa(m_project, {105, 21, 102, 107}));
    QCOMPARE(m_store->projectKeptTaxonIds(m_project), (QSet<qint64>{105, 21, 102, 107}));
    const QStringList found = describe(find({101}));
    QVERIFY(found.contains(QStringLiteral("renamed: Petroica oldname → Microeca newname")));
    QVERIFY(!found.contains(QStringLiteral("not recorded: Petroica vagrans")));
    QVERIFY(!found.contains(QStringLiteral("empty: Emptyia")));
    QVERIFY(!found.contains(QStringLiteral("subspecies: Petroica phoenicea minor")));
}

// A genus the species list itself gave (observations named only that far)
// isn't empty; nor is one whose species sits under a section the tree leaves
// out.
void TestTreeChangeFinder::groupsAreEmptyOnlyWhenNothingIsBeneath()
{
    taxon(22, "genus", "Abies", 10, false);   // listed itself
    taxon(23, "genus", "Eucalyptus", 10, false);
    Taxon section;
    section.inatId = 30;
    section.rank = QStringLiteral("section");
    section.name = QStringLiteral("Adnataria");
    section.parentInatId = 23;
    section.ancestry = QStringLiteral("10/23");
    QVERIFY(m_store->upsertTaxon(section) > 0);   // in the cache, not the tree
    Taxon box;
    box.inatId = 301;
    box.rank = QStringLiteral("species");
    box.name = QStringLiteral("Eucalyptus melliodora");
    box.parentInatId = 30;
    box.ancestry = QStringLiteral("10/23/30");
    QVERIFY(m_store->upsertTaxon(box) > 0);
    QVERIFY(m_store->addProjectTaxon(m_project, 301, true, false));
    now(301, "species", "Eucalyptus melliodora");

    const QStringList found = describe(find({101, 22, 301}));
    QVERIFY(found.contains(QStringLiteral("empty: Emptyia")));
    QVERIFY(!found.contains(QStringLiteral("empty: Abies")));
    QVERIFY(!found.contains(QStringLiteral("empty: Eucalyptus")));
}

// Moving a renamed taxon's photos: matches (the user's too, still theirs),
// learned names; a photo matched to both keeps the new match with the user's
// decision on it.
void TestTreeChangeFinder::movingPhotosToTheNewNameKeepsTheUsersDecisions()
{
    Taxon fresh;
    fresh.inatId = 201;
    fresh.rank = QStringLiteral("species");
    fresh.name = QStringLiteral("Microeca newname");
    QVERIFY(m_store->upsertTaxon(fresh) > 0);
    const qint64 oldId = *m_store->taxonLocalId(102);
    const qint64 newId = *m_store->taxonLocalId(201);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/p', 'p', 0)")));
    for (int c = 1; c <= 3; ++c)
        QVERIFY(q.exec(QStringLiteral("INSERT INTO capture (id, folder_id, base_name) VALUES (%1, 1, 'P%1')").arg(c)));
    // 1: confirmed by the user; 2: auto; 3: auto on the new name, confirmed on the old.
    QVERIFY(q.exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                                  "VALUES (1, %1, 'user', 1, 'confirmed', 'user'), (2, %1, 'file', 0.95, 'auto', 'engine'), "
                                  "(3, %1, 'user', 1, 'confirmed', 'user'), (3, %2, 'file', 0.9, 'auto', 'engine')")
                       .arg(oldId).arg(newId)));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO name_alias (raw_text, raw_folded, taxon_id) VALUES ('Old Robin', 'old robin', %1)").arg(oldId)));

    QCOMPARE(m_store->moveTaxonReferences(102, 201), 3);

    QVERIFY(q.exec(QStringLiteral("SELECT capture_id, status, decided_by FROM capture_match WHERE taxon_id = %1 ORDER BY capture_id").arg(newId)));
    QStringList rows;
    while (q.next())
        rows << QStringLiteral("%1 %2 %3").arg(q.value(0).toInt()).arg(q.value(1).toString(), q.value(2).toString());
    QCOMPARE(rows, (QStringList{QStringLiteral("1 confirmed user"), QStringLiteral("2 auto engine"),
                                QStringLiteral("3 confirmed user")}));
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM capture_match WHERE taxon_id = %1").arg(oldId)) && q.next());
    QCOMPARE(q.value(0).toInt(), 0);
    QVERIFY(q.exec(QStringLiteral("SELECT taxon_id FROM name_alias WHERE raw_folded = 'old robin'")) && q.next());
    QCOMPARE(q.value(0).toLongLong(), newId);
}

QTEST_GUILESS_MAIN(TestTreeChangeFinder)
#include "tst_treechangefinder.moc"
