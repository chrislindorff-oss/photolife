#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include "db/Database.h"
#include "model/ReferencePhotoModel.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::model;
using namespace pl::taxonomy;

// Focus: setProject() (still synchronous) and setScope() (asynchronous,
// background-worker path -- see CaptureListModel's equivalent tests for the
// same fix applied to the taxon-scoped capture grids).
class TestReferencePhotoModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void setProjectLoadsSynchronously();
    void setScopeReloadsAsynchronously();
    void rapidSetScopeCallsOnlyApplyTheLatest();

private:
    std::unique_ptr<QTemporaryDir> m_dbDir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QTemporaryDir> m_photoDir;
    std::unique_ptr<net::PhotoCache> m_photos;
    int m_project = 0;

    void seed();
};

void TestReferencePhotoModel::init()
{
    m_dbDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dbDir->isValid());
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(m_dbDir->filePath(QStringLiteral("catalogue.sqlite"))),
             qPrintable(m_db->error()));
    m_photoDir = std::make_unique<QTemporaryDir>();
    m_photos = std::make_unique<net::PhotoCache>(m_photoDir->path(), QByteArrayLiteral("test"));
    seed();
}

void TestReferencePhotoModel::cleanup()
{
    m_photos.reset();
    m_photoDir.reset();
    m_db.reset();
    m_dbDir.reset();
}

void TestReferencePhotoModel::seed()
{
    TaxonomyStore store(m_db->connectionName());

    Taxon genus;
    genus.inatId = 60815;
    genus.rank = QStringLiteral("genus");
    genus.rankLevel = 20;
    genus.name = QStringLiteral("Diuris");
    QVERIFY(store.upsertTaxon(genus) > 0);

    Taxon sp1;
    sp1.inatId = 900;
    sp1.parentInatId = 60815;
    sp1.rank = QStringLiteral("species");
    sp1.rankLevel = 10;
    sp1.name = QStringLiteral("Diuris pardina");
    QVERIFY(store.upsertTaxon(sp1) > 0);

    Taxon sp2;
    sp2.inatId = 901;
    sp2.parentInatId = 60815;
    sp2.rank = QStringLiteral("species");
    sp2.rankLevel = 10;
    sp2.name = QStringLiteral("Diuris punctata");
    QVERIFY(store.upsertTaxon(sp2) > 0);

    m_project = store.ensureProject(QStringLiteral("Diuris"), 60815, std::nullopt,
                                    QStringLiteral("inat"));
    QVERIFY(m_project > 0);
    QVERIFY(store.addProjectTaxon(m_project, 60815, false, false));
    QVERIFY(store.addProjectTaxon(m_project, 900, true, false));
    QVERIFY(store.addProjectTaxon(m_project, 901, true, false));
}

void TestReferencePhotoModel::setProjectLoadsSynchronously()
{
    ReferencePhotoModel model(*m_db, *m_photos, this);
    model.setProject(m_project);   // no wait: still a direct, synchronous reload()
    QCOMPARE(model.speciesCount(), 2);
}

void TestReferencePhotoModel::setScopeReloadsAsynchronously()
{
    ReferencePhotoModel model(*m_db, *m_photos, this);
    model.setProject(m_project);
    QCOMPARE(model.speciesCount(), 2);

    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setScope(900);   // narrow to one species
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.speciesCount(), 1);
    QCOMPARE(model.index(0).data(ReferencePhotoModel::InatIdRole).toLongLong(), qint64(900));
}

void TestReferencePhotoModel::rapidSetScopeCallsOnlyApplyTheLatest()
{
    ReferencePhotoModel model(*m_db, *m_photos, this);
    model.setProject(m_project);

    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setScope(900);   // superseded before it can be applied
    model.setScope(901);   // the latest request

    QVERIFY(spy.wait(2000));
    QCOMPARE(model.speciesCount(), 1);
    QCOMPARE(model.index(0).data(ReferencePhotoModel::InatIdRole).toLongLong(), qint64(901));

    QTest::qWait(200);   // let any (incorrectly superseding) stale reply land
    QCOMPARE(model.speciesCount(), 1);
    QCOMPARE(model.index(0).data(ReferencePhotoModel::InatIdRole).toLongLong(), qint64(901));
}

QTEST_MAIN(TestReferencePhotoModel)
#include "tst_referencephotomodel.moc"
