#include <QtTest>

#include <QAbstractItemModelTester>
#include <QSet>
#include <QTreeView>

#include "catalogue/SeenStore.h"
#include "db/Database.h"
#include "model/TaxonomyTreeModel.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/Theme.h"

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
    void hiddenRanksReparentChildrenToNearestVisibleAncestor();
    void statusFilterHidesNonMatchingSubtreesAndFindsListedTaxa();
    void visibleTaxonIdsReflectsActiveFilters();
    void coverageFilterCompleteKeepsAncestorsAsContext();
    void coverageFilterIncompleteHidesCompleteBranches();
    void coverageFilterIgnoresHiddenRanks();
    void coverageFilterCombinesWithPhotographedOnly();
    void coverageFilterFollowsCoverageUpdates();
    void threatenedFilterColoursPhotographedGreenAndMissingRed();
    void introducedFilterShowsIntroducedSpeciesInBlueWithAsterisk();
    void groupCountsFollowTheThreatenedAndIntroducedFilters();
    void seenColumnTicksPhotographedAndLetsTheRestBeTicked();
    void seenTickHoldsInEveryTree();
    void seenSubspeciesTicksItsSpecies();
    void seenTotalsFollowTheFilters();
    void seenEditGuardCanRefuse();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    int m_projectId = -1;

    void seedProject();
};

namespace {

// Diuris (1 / 1) is fully covered, Pterostylis (0 / 1) isn't, so the family
// is at 1 / 2.
coverage::ProjectCoverage partialCoverage()
{
    coverage::ProjectCoverage cov;
    auto set = [&](qint64 id, int total, int with) {
        auto &c = cov.byTaxon[id];
        c.speciesTotal = total;
        c.speciesWithPhotos = with;
        c.subtreeHasPhotos = with > 0;
    };
    set(47217, 2, 1);
    set(800, 1, 1);
    set(900, 1, 1);
    set(801, 1, 0);
    set(901, 1, 0);
    return cov;
}

QString nameAt(const QAbstractItemModel &m, const QModelIndex &idx)
{
    return m.data(idx, model::TaxonomyTreeModel::NameRole).toString();
}

bool contextAt(const QAbstractItemModel &m, const QModelIndex &idx)
{
    return m.data(idx, model::TaxonomyTreeModel::ContextOnlyRole).toBool();
}

} // namespace

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

    // A hybrid stored with the multiplication sign (as iNaturalist names it)
    // is still found when the user types a plain "x" for the hybrid marker.
    m_store->upsertTaxon(taxon(902, 800, QStringLiteral("hybrid"), 5,
                               QString::fromUtf8("Diuris \xC3\x97 fallax")));
    m_store->addProjectTaxon(m_projectId, 902, false, false);
    model.setProject(m_projectId);   // re-load to pick up the new project member
    QCOMPARE(model.findTaxa(QStringLiteral("Diuris x fallax")), QList<qint64>{qint64(902)});

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

void TestTaxonomyTreeModel::hiddenRanksReparentChildrenToNearestVisibleAncestor()
{
    model::TaxonomyTreeModel model(*m_db);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setProject(m_projectId);

    QCOMPARE(model.availableRanks(),
             QStringList({QStringLiteral("family"), QStringLiteral("genus"),
                          QStringLiteral("species")}));

    model.setHiddenRanks({QStringLiteral("genus")});

    QCOMPARE(model.rowCount(), 1);   // family is still the sole root
    const QModelIndex family = model.index(0, 0);
    QCOMPARE(model.data(family, Qt::DisplayRole).toString(), QStringLiteral("Orchidaceae"));
    QCOMPARE(model.rowCount(family), 2);           // both species now sit directly under family
    QVERIFY(!model.indexForTaxon(800).isValid());  // Diuris (genus) no longer materialized
    QVERIFY(!model.indexForTaxon(801).isValid());  // Pterostylis likewise
    QVERIFY(model.indexForTaxon(900).isValid());
    QCOMPARE(model.parent(model.indexForTaxon(900)), family);

    model.setHiddenRanks({});   // invalidates `family`; re-fetch before touching the model again
    QVERIFY(model.indexForTaxon(800).isValid());   // genus level restored
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);
}

void TestTaxonomyTreeModel::statusFilterHidesNonMatchingSubtreesAndFindsListedTaxa()
{
    // An infraspecific taxon under Diuris pardina (900) carrying its own
    // status: taxaWithStatus()/anyThreatenedTaxa() include it.
    m_store->upsertTaxon(taxon(950, 900, QStringLiteral("variety"), 5,
                               QStringLiteral("Diuris pardina var. x")));
    m_store->addProjectTaxon(m_projectId, 950, false, false);

    coverage::ProjectCoverage cov;
    cov.byTaxon[900].status = QStringLiteral("Endangered");
    cov.byTaxon[900].subtreeThreatened = true;
    cov.byTaxon[900].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[800].subtreeThreatened = true;
    cov.byTaxon[800].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[47217].subtreeThreatened = true;
    cov.byTaxon[47217].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[950].status = QStringLiteral("Vulnerable");   // infraspecific

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);

    QCOMPARE(model.taxaWithStatus(QStringLiteral("Endangered")), QList<qint64>{qint64(900)});
    QCOMPARE(model.taxaWithStatus(QStringLiteral("Vulnerable")), QList<qint64>{qint64(950)});
    QCOMPARE(model.anyThreatenedTaxa(), (QList<qint64>{qint64(900), qint64(950)}));

    model.setStatusFilter(QStringLiteral("Endangered"));
    QCOMPARE(model.rowCount(), 1);          // family survives
    const QModelIndex family = model.index(0, 0);
    QCOMPARE(model.rowCount(family), 1);    // only the Diuris genus survives
    QCOMPARE(model.data(model.index(0, 0, family), Qt::DisplayRole).toString(),
             QStringLiteral("Diuris"));
    QVERIFY(model.indexForTaxon(900).isValid());
    QVERIFY(!model.indexForTaxon(801).isValid());   // Pterostylis hidden

    model.setStatusFilter(QString());
    QVERIFY(model.indexForTaxon(801).isValid());    // restored

    model.setThreatenedOnly(true);
    QVERIFY(model.indexForTaxon(900).isValid());
    QVERIFY(!model.indexForTaxon(801).isValid());

    // photographedOnly ANDs with threatenedOnly: a status match with no
    // photos is still hidden once photographedOnly is also on.
    model.setPhotographedOnly(true);
    QVERIFY(!model.indexForTaxon(900).isValid());
}

void TestTaxonomyTreeModel::visibleTaxonIdsReflectsActiveFilters()
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

    auto ids = [&model] {
        const QList<qint64> list = model.visibleTaxonIds();
        return QSet<qint64>(list.begin(), list.end());
    };

    QCOMPARE(ids(), (QSet<qint64>{47217, 800, 801, 900, 901}));   // no filters: everything

    model.setPhotographedOnly(true);
    QCOMPARE(ids(), (QSet<qint64>{47217, 800, 900}));   // Pterostylis's unphotographed subtree gone
    model.setPhotographedOnly(false);

    model.setHiddenRanks({QStringLiteral("genus")});
    QCOMPARE(ids(), (QSet<qint64>{47217, 900, 901}));   // both genera collapsed out of the tree
}

void TestTaxonomyTreeModel::coverageFilterCompleteKeepsAncestorsAsContext()
{
    model::TaxonomyTreeModel model(*m_db);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Complete);

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex family = model.index(0, 0);
    QCOMPARE(nameAt(model, family), QStringLiteral("Orchidaceae"));
    QVERIFY(contextAt(model, family));   // 1 / 2: only the path to Diuris
    QVERIFY(model.data(family, Qt::ForegroundRole).isValid());   // drawn muted

    QCOMPARE(model.rowCount(family), 1);
    const QModelIndex diuris = model.index(0, 0, family);
    QCOMPARE(nameAt(model, diuris), QStringLiteral("Diuris"));
    QVERIFY(!contextAt(model, diuris));
    QVERIFY(!model.data(diuris, Qt::ForegroundRole).isValid());
    QCOMPARE(model.rowCount(diuris), 1);   // Diuris pardina, photographed
    QVERIFY(!contextAt(model, model.index(0, 0, diuris)));

    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::All);
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);
    QVERIFY(!contextAt(model, model.index(0, 0)));
}

void TestTaxonomyTreeModel::coverageFilterIncompleteHidesCompleteBranches()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Incomplete);

    QCOMPARE(model.rowCount(), 1);
    const QModelIndex family = model.index(0, 0);
    QVERIFY(!contextAt(model, family));   // 1 / 2 is itself incomplete
    QCOMPARE(model.rowCount(family), 1);
    const QModelIndex pterostylis = model.index(0, 0, family);
    QCOMPARE(nameAt(model, pterostylis), QStringLiteral("Pterostylis"));
    QCOMPARE(model.rowCount(pterostylis), 1);
    QCOMPARE(nameAt(model, model.index(0, 0, pterostylis)), QStringLiteral("Pterostylis nutans"));
    QVERIFY(!model.indexForTaxon(800).isValid());   // Diuris is complete
}

void TestTaxonomyTreeModel::coverageFilterIgnoresHiddenRanks()
{
    // Diuris now has a second, unphotographed species (not in the tree), so
    // it's incomplete even though its one listed species is photographed.
    coverage::ProjectCoverage cov = partialCoverage();
    cov.byTaxon[800].speciesTotal = 2;
    cov.byTaxon[47217].speciesTotal = 3;

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);
    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Complete);

    // With species shown, the photographed Diuris pardina matches, with
    // Diuris and the family as its path.
    QVERIFY(model.indexForTaxon(900).isValid());
    QVERIFY(contextAt(model, model.indexForTaxon(800)));

    // With species hidden, only family and genus rows are filtered: neither
    // is complete, so nothing is left -- not an empty context-only Diuris.
    model.setHiddenRanks({QStringLiteral("species")});
    QCOMPARE(model.rowCount(), 0);

    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Incomplete);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);   // both genera are incomplete
}

void TestTaxonomyTreeModel::coverageFilterCombinesWithPhotographedOnly()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    model.setPhotographedOnly(true);
    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Incomplete);

    // Partly covered only: the family (1 / 2). Pterostylis has no photos;
    // Diuris is complete.
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(nameAt(model, model.index(0, 0)), QStringLiteral("Orchidaceae"));
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
}

void TestTaxonomyTreeModel::coverageFilterFollowsCoverageUpdates()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    model.setCoverageFilter(model::TaxonomyTreeModel::CoverageFilter::Incomplete);
    QVERIFY(model.indexForTaxon(901).isValid());

    // Pterostylis nutans gets photographed: everything is now complete.
    coverage::ProjectCoverage cov = partialCoverage();
    for (qint64 id : {qint64(801), qint64(901)}) {
        cov.byTaxon[id].speciesWithPhotos = 1;
        cov.byTaxon[id].subtreeHasPhotos = true;
    }
    cov.byTaxon[47217].speciesWithPhotos = 2;
    model.setCoverage(cov);
    QCOMPARE(model.rowCount(), 0);
}

void TestTaxonomyTreeModel::threatenedFilterColoursPhotographedGreenAndMissingRed()
{
    // Diuris pardina (900): Endangered, photographed. Pterostylis nutans
    // (901): Vulnerable, not photographed.
    coverage::ProjectCoverage cov = partialCoverage();
    cov.byTaxon[900].status = QStringLiteral("Endangered");
    cov.byTaxon[901].status = QStringLiteral("Vulnerable");
    for (qint64 id : {qint64(47217), qint64(800), qint64(801), qint64(900), qint64(901)})
        cov.byTaxon[id].subtreeThreatened = true;
    cov.byTaxon[47217].subtreeStatuses = {QStringLiteral("Endangered"), QStringLiteral("Vulnerable")};
    cov.byTaxon[800].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[900].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[801].subtreeStatuses = {QStringLiteral("Vulnerable")};
    cov.byTaxon[901].subtreeStatuses = {QStringLiteral("Vulnerable")};

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);
    const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
    auto fg = [&](qint64 id, int column) {
        const QModelIndex idx = model.indexForTaxon(id);
        return model.data(idx.siblingAtColumn(column), Qt::ForegroundRole);
    };
    auto mark = [&](qint64 id) {
        return model.data(model.indexForTaxon(id).siblingAtColumn(2), Qt::DisplayRole).toString();
    };

    // No filter: as before -- names uncoloured, a muted dot for missing.
    QVERIFY(!fg(901, 0).isValid());
    QCOMPARE(mark(901), QStringLiteral("·"));
    QCOMPARE(fg(901, 2).value<QColor>(), theme.mutedText);

    model.setThreatenedOnly(true);
    QCOMPARE(fg(900, 0).value<QColor>(), theme.positive);
    QCOMPARE(fg(900, 2).value<QColor>(), theme.positive);
    QCOMPARE(mark(900), QStringLiteral("✔"));
    QCOMPARE(fg(901, 0).value<QColor>(), theme.danger);
    QCOMPARE(fg(901, 2).value<QColor>(), theme.danger);
    QCOMPARE(mark(901), QStringLiteral("✗"));
    QVERIFY(!fg(800, 0).isValid());   // higher ranks keep their usual colour

    // One tier: only its species are coloured.
    model.setThreatenedOnly(false);
    model.setStatusFilter(QStringLiteral("Vulnerable"));
    QVERIFY(!model.indexForTaxon(900).isValid());   // filtered out
    QCOMPARE(fg(901, 0).value<QColor>(), theme.danger);
    QCOMPARE(mark(901), QStringLiteral("✗"));
}

void TestTaxonomyTreeModel::introducedFilterShowsIntroducedSpeciesInBlueWithAsterisk()
{
    // Diuris pardina (900) is introduced and photographed; Pterostylis
    // nutans (901) is native.
    coverage::ProjectCoverage cov = partialCoverage();
    cov.byTaxon[900].introduced = true;
    for (qint64 id : {qint64(47217), qint64(800), qint64(900)})
        cov.byTaxon[id].subtreeIntroduced = true;

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);
    QCOMPARE(model.introducedTaxa(), QList<qint64>{qint64(900)});
    const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
    auto mark = [&](qint64 id) {
        return model.data(model.indexForTaxon(id).siblingAtColumn(2), Qt::DisplayRole).toString();
    };

    // Filter off: no asterisk, no blue.
    QCOMPARE(mark(900), QStringLiteral("✔"));
    QVERIFY(!model.data(model.indexForTaxon(900), Qt::ForegroundRole).isValid());

    model.setIntroducedOnly(true);
    QVERIFY(model.indexForTaxon(900).isValid());
    QVERIFY(!model.indexForTaxon(901).isValid());   // native: hidden
    QVERIFY(!model.indexForTaxon(801).isValid());
    QCOMPARE(mark(900), QStringLiteral("✔ *"));
    QCOMPARE(model.data(model.indexForTaxon(900), Qt::ForegroundRole).value<QColor>(),
             theme.introduced);
    QCOMPARE(model.data(model.indexForTaxon(900).siblingAtColumn(2), Qt::ForegroundRole)
                 .value<QColor>(),
             theme.introduced);
    QVERIFY(!model.data(model.indexForTaxon(800), Qt::ForegroundRole).isValid());   // genus

    // Unphotographed introduced species: just the asterisk.
    cov.byTaxon[900].subtreeHasPhotos = false;
    model.setCoverage(cov);
    QCOMPARE(mark(900), QStringLiteral("*"));
}

void TestTaxonomyTreeModel::groupCountsFollowTheThreatenedAndIntroducedFilters()
{
    // Diuris pardina (900): Endangered, photographed, introduced (for the
    // test). Pterostylis nutans (901): Vulnerable, not photographed.
    coverage::ProjectCoverage cov = partialCoverage();
    cov.byTaxon[900].status = QStringLiteral("Endangered");
    cov.byTaxon[901].status = QStringLiteral("Vulnerable");
    for (qint64 id : {qint64(47217), qint64(800), qint64(801), qint64(900), qint64(901)})
        cov.byTaxon[id].subtreeThreatened = true;
    cov.byTaxon[47217].subtreeStatuses = {QStringLiteral("Endangered"), QStringLiteral("Vulnerable")};
    cov.byTaxon[800].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[900].subtreeStatuses = {QStringLiteral("Endangered")};
    cov.byTaxon[801].subtreeStatuses = {QStringLiteral("Vulnerable")};
    cov.byTaxon[901].subtreeStatuses = {QStringLiteral("Vulnerable")};
    cov.byTaxon[901].introduced = true;
    for (qint64 id : {qint64(47217), qint64(801), qint64(901)})
        cov.byTaxon[id].subtreeIntroduced = true;

    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(cov);
    auto counts = [&](qint64 id) {
        return model.data(model.indexForTaxon(id).siblingAtColumn(model::TaxonomyTreeModel::CoverageColumn),
                          Qt::DisplayRole).toString();
    };
    auto heading = [&] {
        return model.headerData(model::TaxonomyTreeModel::CoverageColumn, Qt::Horizontal, Qt::DisplayRole).toString();
    };

    // No filter: every species.
    QCOMPARE(counts(47217), QStringLiteral("1 / 2"));
    QCOMPARE(heading(), QStringLiteral("Coverage"));

    // Introduced: only the introduced species count -- 0 / 1, not 1 / 2.
    model.setIntroducedOnly(true);
    QCOMPARE(counts(47217), QStringLiteral("0 / 1"));
    QCOMPARE(counts(801), QStringLiteral("0 / 1"));
    QCOMPARE(heading(), QStringLiteral("Coverage: introduced"));
    QVERIFY(model.data(model.indexForTaxon(47217).siblingAtColumn(2), Qt::ToolTipRole)
                .toString().contains(QStringLiteral("introduced species")));
    model.setIntroducedOnly(false);

    // Threatened: both listed species.
    model.setThreatenedOnly(true);
    QCOMPARE(counts(47217), QStringLiteral("1 / 2"));
    QCOMPARE(counts(800), QStringLiteral("1 / 1"));
    QCOMPARE(heading(), QStringLiteral("Coverage: threatened"));
    model.setThreatenedOnly(false);

    // One tier.
    model.setStatusFilter(QStringLiteral("Vulnerable"));
    QCOMPARE(counts(47217), QStringLiteral("0 / 1"));
    QCOMPARE(heading(), QStringLiteral("Coverage: Vulnerable"));
    model.setStatusFilter(QString());

    // Back to every species.
    QCOMPARE(counts(47217), QStringLiteral("1 / 2"));
    QCOMPARE(heading(), QStringLiteral("Coverage"));
}

namespace {

QModelIndex seenCell(const model::TaxonomyTreeModel &m, qint64 inatId)
{
    const QModelIndex idx = m.indexForTaxon(inatId);
    return idx.sibling(idx.row(), model::TaxonomyTreeModel::SeenColumn);
}

} // namespace

void TestTaxonomyTreeModel::seenColumnTicksPhotographedAndLetsTheRestBeTicked()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());   // Diuris pardina (900) photographed
    QCOMPARE(model.columnCount(), 4);
    QCOMPARE(model.headerData(model::TaxonomyTreeModel::SeenColumn, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Seen"));

    const QModelIndex pardina = seenCell(model, 900);
    QCOMPARE(pardina.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(!(model.flags(pardina) & Qt::ItemIsEnabled));   // fixed: it's photographed
    QVERIFY(!model.setData(pardina, Qt::Unchecked, Qt::CheckStateRole));

    const QModelIndex nutans = seenCell(model, 901);
    QCOMPARE(nutans.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    QVERIFY(model.flags(nutans) & Qt::ItemIsUserCheckable);
    QVERIFY(model.flags(nutans) & Qt::ItemIsEnabled);

    const QModelIndex family = seenCell(model, 47217);
    QVERIFY(!family.data(Qt::CheckStateRole).isValid());
    QCOMPARE(family.data(Qt::DisplayRole).toString(), QStringLiteral("1 / 2"));

    QSignalSpy changed(&model, &model::TaxonomyTreeModel::seenChanged);
    QVERIFY(model.setData(nutans, Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(seenCell(model, 901).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QCOMPARE(seenCell(model, 47217).data(Qt::DisplayRole).toString(), QStringLiteral("2 / 2"));
    QVERIFY(catalogue::SeenStore(m_db->connectionName()).seenTaxa().contains(901));

    QVERIFY(model.setData(seenCell(model, 901), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(catalogue::SeenStore(m_db->connectionName()).seenTaxa().isEmpty());
    QCOMPARE(seenCell(model, 47217).data(Qt::DisplayRole).toString(), QStringLiteral("1 / 2"));
}

void TestTaxonomyTreeModel::seenTickHoldsInEveryTree()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    QVERIFY(model.setMarkedSeen(901, true));

    // Another tree with the same species.
    const int other = m_store->ensureProject(QStringLiteral("Other"), 801, std::nullopt, QStringLiteral("inat"));
    m_store->addProjectTaxon(other, 801, false, false);
    m_store->addProjectTaxon(other, 901, false, false);
    model::TaxonomyTreeModel second(*m_db);
    second.setProject(other);
    QCOMPARE(seenCell(second, 901).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(second.seenState(901).marked);
}

void TestTaxonomyTreeModel::seenSubspeciesTicksItsSpecies()
{
    m_store->upsertTaxon(taxon(902, 901, QStringLiteral("subspecies"), 5, QStringLiteral("Pterostylis nutans alba")));
    m_store->addProjectTaxon(m_projectId, 902, false, false);
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());

    QVERIFY(model.setMarkedSeen(902, true));
    const coverage::SeenState nutans = model.seenState(901);
    QVERIFY(nutans.seen);
    QVERIFY(!nutans.marked);
    QCOMPARE(nutans.seenVia, qint64(902));
    QVERIFY(!model.canMarkSeen(901));   // seen through its subspecies: untick that instead
    QVERIFY(!(model.flags(seenCell(model, 901)) & Qt::ItemIsEnabled));
    QVERIFY(seenCell(model, 901).data(Qt::ToolTipRole).toString().contains(QStringLiteral("alba")));

    const coverage::SeenTotals t = model.seenTotals();
    QCOMPARE(t.speciesShown, 2);
    QCOMPARE(t.speciesSeen, 2);
    QCOMPARE(t.speciesPhotographed, 1);
    QCOMPARE(t.speciesSeenOnly, 1);
    QCOMPARE(t.belowSeen, 1);
}

void TestTaxonomyTreeModel::seenTotalsFollowTheFilters()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    QVERIFY(model.setMarkedSeen(901, true));
    QCOMPARE(model.seenTotals().speciesShown, 2);
    QCOMPARE(model.seenTotals().speciesSeen, 2);

    QSignalSpy changed(&model, &model::TaxonomyTreeModel::seenChanged);
    model.setPhotographedOnly(true);
    QVERIFY(changed.count() >= 1);
    QCOMPARE(model.seenTotals().speciesShown, 1);
    QCOMPARE(model.seenTotals().speciesSeen, 1);
    QCOMPARE(model.seenTotals().speciesSeenOnly, 0);

    // A tick hidden by a filter can't be changed from here.
    QVERIFY(!model.canMarkSeen(901));
}

void TestTaxonomyTreeModel::seenEditGuardCanRefuse()
{
    model::TaxonomyTreeModel model(*m_db);
    model.setProject(m_projectId);
    model.setCoverage(partialCoverage());
    int asked = 0;
    model.setEditGuard([&] {
        ++asked;
        return false;
    });
    QVERIFY(!model.setData(seenCell(model, 901), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(asked, 1);
    QVERIFY(catalogue::SeenStore(m_db->connectionName()).seenTaxa().isEmpty());
    QCOMPARE(seenCell(model, 901).data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
}

QTEST_MAIN(TestTaxonomyTreeModel)
#include "tst_taxonomytreemodel.moc"
