#include <QtTest>

#include <QAbstractItemModelTester>
#include <QTreeView>

#include "db/Database.h"
#include "model/TaxonomyTreeModel.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::taxonomy;

namespace {

Taxon taxon(qint64 id, qint64 parent, const QString &rank, int rankLevel, const QString &name)
{
    Taxon t;
    t.inatId = id;
    if (parent > 0)
        t.parentInatId = parent;
    t.rank = rank;
    t.rankLevel = rankLevel;
    t.name = name;
    return t;
}

} // namespace

class TestTaxonomyTreeModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void buildsHierarchyFromFlatRows();
    void passesModelTester();
    void emptyProjectHasNoRows();
    void photographedOnlyFilterHidesUnphotographedSubtrees();
    void indexForTaxonReflectsVisibility();
    void filterToggleKeepsExpansionAndSelection();
    void findTaxaMatchesNameAndCommonName();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    int m_projectId = -1;

    void seedProject();
};

void TestTaxonomyTreeModel::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());
    seedProject();
}

void TestTaxonomyTreeModel::cleanup()
{
    m_store.reset();
    m_db.reset();
}

void TestTaxonomyTreeModel::seedProject()
{
    m_store->upsertTaxon(taxon(47217, 0, QStringLiteral("family"), 30, QStringLiteral("Orchidaceae")));
    m_store->upsertTaxon(taxon(800, 47217, QStringLiteral("genus"), 20, QStringLiteral("Diuris")));
    m_store->upsertTaxon(taxon(801, 47217, QStringLiteral("genus"), 20, QStringLiteral("Pterostylis")));
    Taxon sp = taxon(900, 800, QStringLiteral("species"), 10, QStringLiteral("Diuris pardina"));
    sp.commonName = QStringLiteral("Leopard Orchid");
    m_store->upsertTaxon(sp);
    m_store->upsertTaxon(taxon(901, 801, QStringLiteral("species"), 10, QStringLiteral("Pterostylis nutans")));

    m_projectId = m_store->ensureProject(QStringLiteral("Test"), 47217, std::nullopt,
                                         QStringLiteral("inat"));
    for (qint64 id : {qint64(47217), qint64(800), qint64(801), qint64(900)})
        m_store->addProjectTaxon(m_projectId, id, false, false);
    m_store->addProjectTaxon(m_projectId, 901, true, false);   // in region
}

void TestTaxonomyTreeModel::buildsHierarchyFromFlatRows()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);

    QCOMPARE(model.rowCount(), 1);   // one root: the family
    const QModelIndex family = model.index(0, 0);
    QCOMPARE(model.data(family, Qt::DisplayRole).toString(), QStringLiteral("Orchidaceae"));
    QCOMPARE(model.data(family, model::TaxonomyTreeModel::RankRole).toString(),
             QStringLiteral("family"));
    QCOMPARE(model.rowCount(family), 2);   // two genera

    const QModelIndex diuris = model.index(0, 0, family);
    QCOMPARE(model.data(diuris, Qt::DisplayRole).toString(), QStringLiteral("Diuris"));
    QCOMPARE(model.rowCount(diuris), 1);

    const QModelIndex pardina = model.index(0, 0, diuris);
    QCOMPARE(model.data(pardina, Qt::DisplayRole).toString(),
             QStringLiteral("Diuris pardina  ·  Leopard Orchid"));
    QCOMPARE(model.parent(pardina), diuris);
    QCOMPARE(model.parent(diuris), family);
    QVERIFY(!model.parent(family).isValid());

    // The rank-1 column.
    QCOMPARE(model.data(model.index(0, 1, family), Qt::DisplayRole).toString(),
             QStringLiteral("genus"));

    // in_region species is flagged (bold font role).
    const QModelIndex pterostylis = model.index(1, 0, family);
    const QModelIndex nutans = model.index(0, 0, pterostylis);
    QVERIFY(model.data(nutans, model::TaxonomyTreeModel::InRegionRole).toBool());
    QVERIFY(model.data(nutans, Qt::FontRole).value<QFont>().bold());
}

void TestTaxonomyTreeModel::passesModelTester()
{
    model::TaxonomyTreeModel model(*m_db);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setProject(m_projectId);
    // Re-set to exercise reset paths.
    model.setProject(-1);
    model.setProject(m_projectId);
}

void TestTaxonomyTreeModel::emptyProjectHasNoRows()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(-1);
    QCOMPARE(model.rowCount(), 0);
}

void TestTaxonomyTreeModel::photographedOnlyFilterHidesUnphotographedSubtrees()
{
    // Diuris pardina (900) is photographed; Pterostylis nutans (901) is not.
    coverage::ProjectCoverage cov;
    for (qint64 id : {qint64(47217), qint64(800), qint64(900)})
        cov.byTaxon[id].subtreeHasPhotos = true;
    for (qint64 id : {qint64(801), qint64(901)})
        cov.byTaxon[id].subtreeHasPhotos = false;

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);

    QCOMPARE(model.rowCount(), 1);            // full tree: the family
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);   // both genera

    model.setPhotographedOnly(true);
    QCOMPARE(model.rowCount(), 1);            // still the family (it has photos)
    const QModelIndex family = model.index(0, 0);
    QCOMPARE(model.rowCount(family), 1);      // only Diuris survives
    QCOMPARE(model.data(model.index(0, 0, family), Qt::DisplayRole).toString(),
             QStringLiteral("Diuris"));
    QCOMPARE(model.rowCount(model.index(0, 0, family)), 1);   // Diuris pardina

    model.setPhotographedOnly(false);
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);   // Pterostylis is back
}

void TestTaxonomyTreeModel::indexForTaxonReflectsVisibility()
{
    coverage::ProjectCoverage cov;
    for (qint64 id : {qint64(47217), qint64(800), qint64(900)})
        cov.byTaxon[id].subtreeHasPhotos = true;

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);

    const QModelIndex diuris = model.indexForTaxon(800);
    QVERIFY(diuris.isValid());
    QCOMPARE(diuris.data(model::TaxonomyTreeModel::InatIdRole).toLongLong(), qint64(800));
    QVERIFY(model.indexForTaxon(801).isValid());   // Pterostylis, currently shown
    QVERIFY(!model.indexForTaxon(999999).isValid());

    model.setPhotographedOnly(true);
    QVERIFY(model.indexForTaxon(800).isValid());   // Diuris still shown
    QVERIFY(!model.indexForTaxon(801).isValid());  // Pterostylis now hidden

    model.setPhotographedOnly(false);
    QVERIFY(model.indexForTaxon(801).isValid());   // back
}

void TestTaxonomyTreeModel::filterToggleKeepsExpansionAndSelection()
{
    coverage::ProjectCoverage cov;
    for (qint64 id : {qint64(47217), qint64(800), qint64(900)})
        cov.byTaxon[id].subtreeHasPhotos = true;

    model::TaxonomyTreeModel model(*m_db);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setProject(m_projectId);
    model.setCoverage(cov);

    QTreeView view;
    view.setModel(&model);
    view.expand(model.indexForTaxon(47217));   // family
    view.expand(model.indexForTaxon(800));     // Diuris
    view.setCurrentIndex(model.indexForTaxon(900));   // Diuris pardina
    QVERIFY(view.isExpanded(model.indexForTaxon(800)));

    // The MainWindow snapshot/restore, inlined.
    auto toggle = [&](bool on) {
        QList<qint64> expanded;
        for (qint64 id : {qint64(47217), qint64(800), qint64(801), qint64(900), qint64(901)})
            if (const QModelIndex i = model.indexForTaxon(id); i.isValid() && view.isExpanded(i))
                expanded << id;
        const qint64 current =
            view.currentIndex().data(model::TaxonomyTreeModel::InatIdRole).toLongLong();

        model.setPhotographedOnly(on);

        for (qint64 id : expanded)
            if (const QModelIndex i = model.indexForTaxon(id); i.isValid())
                view.setExpanded(i, true);
        if (const QModelIndex c = model.indexForTaxon(current); c.isValid())
            view.setCurrentIndex(c);
    };

    toggle(true);
    QVERIFY(view.isExpanded(model.indexForTaxon(800)));           // Diuris still open
    QCOMPARE(view.currentIndex().data(model::TaxonomyTreeModel::InatIdRole).toLongLong(),
             qint64(900));                                        // still on Diuris pardina
    QVERIFY(!model.indexForTaxon(801).isValid());                 // Pterostylis hidden

    toggle(false);
    QVERIFY(view.isExpanded(model.indexForTaxon(800)));           // still open after untick
    QCOMPARE(view.currentIndex().data(model::TaxonomyTreeModel::InatIdRole).toLongLong(),
             qint64(900));
    QVERIFY(model.indexForTaxon(801).isValid());                  // Pterostylis reappeared
}

void TestTaxonomyTreeModel::findTaxaMatchesNameAndCommonName()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);

    QCOMPARE(model.findTaxa(QString()), QList<qint64>{});

    // Scientific-name prefix.
    const QList<qint64> diuris = model.findTaxa(QStringLiteral("diur"));
    QVERIFY(!diuris.isEmpty());
    QCOMPARE(diuris.first(), qint64(800));   // genus Diuris, a prefix hit, ranks first
    QVERIFY(diuris.contains(qint64(900)));   // Diuris pardina also matches

    // A prefix hit outranks a mid-string hit: "pardina" only matches the species.
    QCOMPARE(model.findTaxa(QStringLiteral("pardina")), QList<qint64>{qint64(900)});

    // Common-name match, case-insensitive.
    QCOMPARE(model.findTaxa(QStringLiteral("leopard")), QList<qint64>{qint64(900)});

    // Hidden taxa are still searchable (findTaxa works off the full project set).
    coverage::ProjectCoverage cov;
    for (qint64 id : {qint64(47217), qint64(800), qint64(900)})
        cov.byTaxon[id].subtreeHasPhotos = true;
    model.setCoverage(cov);
    model.setPhotographedOnly(true);
    QVERIFY(!model.indexForTaxon(901).isValid());                       // Pterostylis hidden
    QCOMPARE(model.findTaxa(QStringLiteral("pterostylis")).first(), qint64(801));

    QVERIFY(model.findTaxa(QStringLiteral("nothing here")).isEmpty());
}

QTEST_MAIN(TestTaxonomyTreeModel)
#include "tst_taxonomytreemodel.moc"
