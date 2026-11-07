#include <QtTest>

#include <QColumnView>
#include <QMouseEvent>

#include "catalogue/SeenStore.h"
#include "db/Database.h"
#include "model/TaxonomyTreeModel.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/TaxonColumnView.h"

using namespace pl;
using namespace pl::taxonomy;
using Model = pl::model::TaxonomyTreeModel;

namespace {

Taxon taxon(qint64 id, qint64 parent, const char *rank, int level, const char *name)
{
    Taxon t;
    t.inatId = id;
    if (parent > 0)
        t.parentInatId = parent;
    t.rank = QString::fromLatin1(rank);
    t.rankLevel = level;
    t.name = QString::fromLatin1(name);
    return t;
}

} // namespace

class TestTaxonColumnView : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void columnZeroCarriesSeenAndCoverage();
    void tickShowsOnlyForSpeciesWhenSeenIsOn();
    void clickingTheTickTogglesSeen();
    void columnViewOpensTheWayDown();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<Model> m_model;
    int m_project = -1;
};

void TestTaxonColumnView::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    TaxonomyStore store(m_db->connectionName());
    store.upsertTaxon(taxon(1, 0, "family", 30, "Acanthizidae"));
    store.upsertTaxon(taxon(10, 1, "genus", 20, "Gerygone"));
    store.upsertTaxon(taxon(100, 10, "species", 10, "Gerygone olivacea"));
    store.upsertTaxon(taxon(101, 10, "species", 10, "Gerygone fusca"));
    m_project = store.ensureProject(QStringLiteral("T"), 1, std::nullopt, QStringLiteral("inat"));
    for (qint64 id : {1, 10, 100, 101})
        store.addProjectTaxon(m_project, id, false, false);
    m_model = std::make_unique<Model>(*m_db);
    m_model->setProject(m_project);
    coverage::ProjectCoverage cov;
    cov.byTaxon[1].speciesTotal = 2;
    cov.byTaxon[1].speciesWithPhotos = 1;
    cov.byTaxon[1].subtreeHasPhotos = true;
    cov.byTaxon[10] = cov.byTaxon[1];
    cov.byTaxon[101].subtreeHasPhotos = true;
    cov.byTaxon[101].speciesTotal = cov.byTaxon[101].speciesWithPhotos = 1;
    cov.byTaxon[101].captureCount = 3;
    m_model->setCoverage(cov);
}

void TestTaxonColumnView::cleanup()
{
    m_model.reset();
    m_db.reset();
}

void TestTaxonColumnView::columnZeroCarriesSeenAndCoverage()
{
    const QModelIndex genus = m_model->indexForTaxon(10);
    QVERIFY(!genus.data(Model::SeenRole).isValid());   // above species: no tick
    const QModelIndex olivacea = m_model->indexForTaxon(100);
    QCOMPARE(olivacea.data(Model::SeenRole).toInt(), int(Qt::Unchecked));
    QVERIFY(olivacea.data(Model::SeenEditableRole).toBool());
    const QModelIndex fusca = m_model->indexForTaxon(101);
    QCOMPARE(fusca.data(Model::SeenRole).toInt(), int(Qt::Checked));   // photographed
    QVERIFY(!fusca.data(Model::SeenEditableRole).toBool());
    QCOMPARE(fusca.data(Model::CaptureCountRole).toInt(), 3);
}

void TestTaxonColumnView::tickShowsOnlyForSpeciesWhenSeenIsOn()
{
    TaxonColumnDelegate delegate;
    const QRect row(0, 0, 300, 26);
    QVERIFY(delegate.checkRect(row, m_model->indexForTaxon(100)).isNull());   // Seen off
    delegate.setShowSeen(true);
    QVERIFY(!delegate.checkRect(row, m_model->indexForTaxon(100)).isNull());
    QVERIFY(delegate.checkRect(row, m_model->indexForTaxon(10)).isNull());    // a genus
    QVERIFY(row.contains(delegate.checkRect(row, m_model->indexForTaxon(100))));
}

void TestTaxonColumnView::clickingTheTickTogglesSeen()
{
    TaxonColumnDelegate delegate;
    delegate.setShowSeen(true);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 300, 26);
    option.font = QApplication::font();
    const QModelIndex olivacea = m_model->indexForTaxon(100);
    const QPoint at = delegate.checkRect(option.rect, olivacea).center();
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), QPointF(at), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QVERIFY(delegate.editorEvent(&release, m_model.get(), option, olivacea));
    QCOMPARE(m_model->indexForTaxon(100).data(Model::SeenRole).toInt(), int(Qt::Checked));
    QVERIFY(catalogue::SeenStore(m_db->connectionName()).seenTaxa().contains(100));

    // A photographed species' tick is fixed.
    const QModelIndex fusca = m_model->indexForTaxon(101);
    QMouseEvent again(QEvent::MouseButtonRelease, QPointF(at), QPointF(at), Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
    delegate.editorEvent(&again, m_model.get(), option, fusca);
    QCOMPARE(m_model->indexForTaxon(101).data(Model::SeenRole).toInt(), int(Qt::Checked));
    QVERIFY(!catalogue::SeenStore(m_db->connectionName()).seenTaxa().contains(101));

    // Away from the tick, a click is just a click.
    QMouseEvent elsewhere(QEvent::MouseButtonRelease, QPointF(20, 10), QPointF(20, 10), Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
    QVERIFY(!delegate.editorEvent(&elsewhere, m_model.get(), option, m_model->indexForTaxon(100)));
    QCOMPARE(m_model->indexForTaxon(100).data(Model::SeenRole).toInt(), int(Qt::Checked));
}

void TestTaxonColumnView::columnViewOpensTheWayDown()
{
    QColumnView view;
    view.setItemDelegate(new TaxonColumnDelegate(&view));
    view.setModel(m_model.get());
    view.resize(1200, 400);
    view.show();
    view.setCurrentIndex(m_model->indexForTaxon(100));
    QCoreApplication::processEvents();
    // Family, genus and species columns (plus the empty preview).
    QList<QAbstractItemView *> lists;
    for (QAbstractItemView *v : view.findChildren<QAbstractItemView *>())
        if (v->rootIndex().isValid() || v->model() == m_model.get())
            lists << v;
    QVERIFY(lists.size() >= 3);
    QCOMPARE(view.currentIndex().data(Model::InatIdRole).toLongLong(), qint64(100));
}

QTEST_MAIN(TestTaxonColumnView)
#include "tst_taxoncolumnview.moc"
