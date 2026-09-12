#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "model/CaptureListModel.h"
#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TaxonomyTypes.h"
#include "thumb/ThumbnailCache.h"

using namespace pl;
using namespace pl::model;

// Focus: the taxon-scope filter, and its interaction with the project scope.
// A reference tree shows ancestor nodes (up to a synthetic "Life" root) purely
// for structure; selecting one must not pull in other trees' photos that share
// that ancestor in the global taxon cache.
class TestCaptureListModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void taxonScopeAloneSpansEveryTreeSharingTheAncestor();
    void projectScopeConfinesTaxonScopeToTheActiveTree();
    void projectScopeAloneConfinesToTheActiveTree();
    void captionLocalityShownOnlyWhenEnabledAndCached();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QTemporaryDir> m_thumbDir;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbs;
    int m_birdProject = 0;

    void seed();
    int addCaptureMatchedTo(const QString &baseName, qint64 taxonInatId);
    QStringList names(CaptureListModel &m) const;
};

void TestCaptureListModel::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
    m_thumbDir = std::make_unique<QTemporaryDir>();
    m_thumbs = std::make_unique<thumb::ThumbnailCache>(m_thumbDir->path());
    seed();
}

void TestCaptureListModel::cleanup()
{
    m_thumbs.reset();
    m_thumbDir.reset();
    m_db.reset();
}

void TestCaptureListModel::seed()
{
    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        t.name = name;
        QVERIFY(store.upsertTaxon(t) > 0);
    };
    // Shared synthetic root, then two unrelated branches.
    add(48460, 0, QStringLiteral("stateofmatter"), QStringLiteral("Life"));
    add(3, 48460, QStringLiteral("class"), QStringLiteral("Aves"));
    add(900, 3, QStringLiteral("genus"), QStringLiteral("Corvus"));
    add(901, 900, QStringLiteral("species"), QStringLiteral("Corvus mellori"));
    add(47126, 48460, QStringLiteral("phylum"), QStringLiteral("Tracheophyta"));
    add(101, 47126, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));

    // The bird tree carries the "Life" ancestor stub, but not the plant taxa.
    m_birdProject = store.ensureProject(QStringLiteral("Birds"), 3, std::nullopt,
                                        QStringLiteral("inat"));
    for (qint64 id : {qint64(48460), qint64(3), qint64(900), qint64(901)})
        QVERIFY(store.addProjectTaxon(m_birdProject, id, false, false));

    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) "
                             "VALUES (1, '/lib', 'lib', 0)"));

    addCaptureMatchedTo(QStringLiteral("crow"), 901);      // in the bird tree
    addCaptureMatchedTo(QStringLiteral("orchid"), 101);    // shares only "Life"
}

int TestCaptureListModel::addCaptureMatchedTo(const QString &baseName, qint64 taxonInatId)
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QSqlQuery cap(db);
    cap.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (1, ?)"));
    cap.addBindValue(baseName);
    cap.exec();
    const int captureId = cap.lastInsertId().toInt();

    QSqlQuery ren(db);
    ren.prepare(QStringLiteral(
        "INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    ren.addBindValue(captureId);
    ren.addBindValue(QStringLiteral("/lib/%1.jpg").arg(baseName));
    ren.exec();

    QSqlQuery m(db);
    m.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
        "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'manual', 1.0, 'confirmed', 'user')"));
    m.addBindValue(captureId);
    m.addBindValue(taxonInatId);
    m.exec();
    return captureId;
}

QStringList TestCaptureListModel::names(CaptureListModel &m) const
{
    QStringList out;
    for (int r = 0; r < m.rowCount(); ++r)
        out << m.index(r, 0).data(CaptureListModel::MatchedNameRole).toString();
    out.sort();
    return out;
}

void TestCaptureListModel::taxonScopeAloneSpansEveryTreeSharingTheAncestor()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    model.setTaxonScope(48460);   // "Life" — no project scope
    QCOMPARE(names(model),
             (QStringList{QStringLiteral("Caladenia carnea"), QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::projectScopeConfinesTaxonScopeToTheActiveTree()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    model.setProjectScope(m_birdProject);

    model.setTaxonScope(48460);   // selecting the "Life" stub in the bird tree
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    model.setTaxonScope(900);     // selecting the genus
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    model.setTaxonScope(101);     // a taxon not in this tree at all
    QCOMPARE(model.rowCount(), 0);
}

void TestCaptureListModel::projectScopeAloneConfinesToTheActiveTree()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    model.reload();
    QCOMPARE(model.rowCount(), 2);          // no scope at all -> whole library

    model.setProjectScope(m_birdProject);   // a tree is active, nothing selected
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    model.setProjectScope(0);               // no tree -> back to the whole library
    QCOMPARE(model.rowCount(), 2);
}

void TestCaptureListModel::captionLocalityShownOnlyWhenEnabledAndCached()
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    const int captureId = addCaptureMatchedTo(QStringLiteral("crow2"), 901);

    QSqlQuery gps(db);
    gps.prepare(QStringLiteral("UPDATE capture SET latitude = ?, longitude = ? WHERE id = ?"));
    gps.addBindValue(-37.8136);
    gps.addBindValue(144.9631);
    gps.addBindValue(captureId);
    QVERIFY(gps.exec());

    QSqlQuery cache(db);
    cache.prepare(QStringLiteral(
        "INSERT INTO geocode_cache (lat_round, lon_round, locality) VALUES (?, ?, ?)"));
    cache.addBindValue(-37.814);   // ROUND(-37.8136, 3)
    cache.addBindValue(144.963);   // ROUND(144.9631, 3)
    cache.addBindValue(QStringLiteral("Melbourne, Victoria, Australia"));
    QVERIFY(cache.exec());

    CaptureListModel model(*m_db, *m_thumbs, this);
    model.setCaptionFields(CaptureListModel::CaptionName | CaptureListModel::CaptionLocality);
    model.reload();

    int row = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.index(r).data(CaptureListModel::IdRole).toInt() == captureId) {
            row = r;
            break;
        }
    }
    QVERIFY(row >= 0);
    const QModelIndex idx = model.index(row);
    QCOMPARE(idx.data(CaptureListModel::LocalityRole).toString(),
             QStringLiteral("Melbourne, Victoria, Australia"));
    QVERIFY(idx.data(Qt::DisplayRole).toString().contains(QStringLiteral("Melbourne")));

    model.setCaptionFields(CaptureListModel::CaptionName);   // Locality unchecked
    QVERIFY(!model.index(row).data(Qt::DisplayRole).toString().contains(QStringLiteral("Melbourne")));
}

QTEST_MAIN(TestCaptureListModel)
#include "tst_capturelistmodel.moc"
