#include <QtTest>

#include "catalogue/DuplicateFinder.h"

#include <QSignalSpy>
#include <QSqlError>
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
    void setScopeCoalescesIntoOneReload();
    void rapidSetScopeCallsOnlyApplyTheLatest();
    void taxonSetScopeMatchesExactIdsNotASubtreeWalk();
    void taxonSetScopeCombinesWithProjectScope();
    void taxonSetScopeAndTaxonScopeAreMutuallyExclusive();
    void captionLocalityShownOnlyWhenEnabledAndCached();
    void applyGpsPatchesRowInPlaceWithoutReset();
    void gpsFilterKeepsOnlyPhotosWithOrWithoutALocation();
    void confirmedOrAutoStatusFilterIncludesBothStatuses();
    void searchMatchesEveryWordAnywhere();
    void duplicatesAreFilesWithTheSameContent();

private:
    std::unique_ptr<QTemporaryDir> m_dbDir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QTemporaryDir> m_thumbDir;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbs;
    int m_birdProject = 0;

    void seed();
    int addCaptureMatchedTo(const QString &baseName, qint64 taxonInatId);
    int addAutoMatchedCapture(const QString &baseName, qint64 taxonInatId);
    QStringList names(CaptureListModel &m) const;
};

void TestCaptureListModel::init()
{
    m_dbDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dbDir->isValid());
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(m_dbDir->filePath(QStringLiteral("catalogue.sqlite"))),
             qPrintable(m_db->error()));
    m_thumbDir = std::make_unique<QTemporaryDir>();
    m_thumbs = std::make_unique<thumb::ThumbnailCache>(m_thumbDir->path());
    seed();
}

void TestCaptureListModel::cleanup()
{
    m_thumbs.reset();
    m_thumbDir.reset();
    m_db.reset();
    m_dbDir.reset();
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

int TestCaptureListModel::addAutoMatchedCapture(const QString &baseName, qint64 taxonInatId)
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
        "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'folder+file', 0.95, 'auto', "
        "'engine')"));
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

void TestCaptureListModel::searchMatchesEveryWordAnywhere()
{
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("UPDATE capture SET captured_on = '2025-10-18T09:30:00' WHERE base_name = 'orchid'"));
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("UPDATE taxon SET common_name = 'Pink Fingers' WHERE inat_id = 101"));
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    auto search = [&](const QString &text) {
        spy.clear();
        model.setSearch(text);
        if (spy.isEmpty())
            spy.wait(2000);
        return names(model);
    };
    const QStringList both{QStringLiteral("Caladenia carnea"), QStringLiteral("Corvus mellori")};
    QCOMPARE(search(QStringLiteral("caladenia")), QStringList{QStringLiteral("Caladenia carnea")});   // scientific name
    QCOMPARE(search(QStringLiteral("PINK")), QStringList{QStringLiteral("Caladenia carnea")});      // common name, any case
    QCOMPARE(search(QStringLiteral("crow")), QStringList{QStringLiteral("Corvus mellori")});        // file name
    QCOMPARE(search(QStringLiteral("2025-10")), QStringList{QStringLiteral("Caladenia carnea")});   // date
    QCOMPARE(search(QStringLiteral("/lib")), both);                                                 // folder
    QCOMPARE(search(QStringLiteral("pink 2025")), QStringList{QStringLiteral("Caladenia carnea")}); // every word
    QVERIFY(search(QStringLiteral("pink corvus")).isEmpty());
    QVERIFY(search(QStringLiteral("100%")).isEmpty());   // % is just a character
    QCOMPARE(search(QString()), both);                    // no search
    // Combines with a scope: the bird tree has no orchid.
    spy.clear();
    model.setProjectScope(m_birdProject);
    spy.wait(2000);
    QVERIFY(search(QStringLiteral("caladenia")).isEmpty());

    QStringList binds;
    QVERIFY(CaptureListModel::searchClause(QStringLiteral("  "), &binds).isEmpty());
    const QString clause = CaptureListModel::searchClause(QStringLiteral("a_b"), &binds);
    QVERIFY(clause.contains(QStringLiteral("ESCAPE")));
    QCOMPARE(binds.first(), QStringLiteral("%a\\_b%"));
}

void TestCaptureListModel::duplicatesAreFilesWithTheSameContent()
{
    QSqlDatabase db = QSqlDatabase::database(m_db->connectionName(), false);
    QVERIFY(catalogue::findDuplicates(m_db->connectionName()).isEmpty());
    auto addFile = [&](int captureId, const QString &path, const QString &hash, qint64 size) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash, file_size) "
                                 "VALUES (?, ?, 'jpeg', 'jpg', ?, ?)"));
        q.addBindValue(captureId);
        q.addBindValue(path);
        q.addBindValue(hash);
        q.addBindValue(size);
        QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    };
    const int a = addCaptureMatchedTo(QStringLiteral("a"), 901);
    const int b = addCaptureMatchedTo(QStringLiteral("b"), 901);
    addFile(a, QStringLiteral("/lib/one/small.jpg"), QStringLiteral("h1"), 100);
    addFile(b, QStringLiteral("/lib/two/small.jpg"), QStringLiteral("h1"), 100);
    addFile(a, QStringLiteral("/lib/one/big.jpg"), QStringLiteral("h2"), 5000);
    addFile(b, QStringLiteral("/lib/two/big copy.jpg"), QStringLiteral("h2"), 5000);
    addFile(b, QStringLiteral("/lib/three/big.jpg"), QStringLiteral("h2"), 5000);
    addFile(a, QStringLiteral("/lib/one/only.jpg"), QStringLiteral("h3"), 700);
    const auto everywhere = [](const QString &) { return true; };
    const QList<catalogue::DuplicateGroup> groups = catalogue::findDuplicates(m_db->connectionName(), nullptr, everywhere);
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.at(0).contentHash, QStringLiteral("h2"));   // the most to free, first
    QCOMPARE(groups.at(0).paths.size(), 3);
    QCOMPARE(groups.at(0).spareBytes(), qint64(10000));
    QCOMPARE(groups.at(1).paths, (QStringList{QStringLiteral("/lib/one/small.jpg"), QStringLiteral("/lib/two/small.jpg")}));
    QCOMPARE(groups.at(1).captureIds, (QList<int>{a, b}));
    // Copies no longer on disk aren't duplicates: one of big.jpg's three is gone,
    // and small.jpg's other copy, so small.jpg has none left.
    int stale = -1;
    const QList<catalogue::DuplicateGroup> onDisk = catalogue::findDuplicates(
        m_db->connectionName(), nullptr,
        [](const QString &path) { return path != QLatin1String("/lib/three/big.jpg") && path != QLatin1String("/lib/two/small.jpg"); },
        &stale);
    QCOMPARE(stale, 2);
    QCOMPARE(onDisk.size(), 1);
    QCOMPARE(onDisk.first().paths.size(), 2);
    // And by default it's the disk that says: none of these made-up paths exist.
    QVERIFY(catalogue::findDuplicates(m_db->connectionName()).isEmpty());
}

void TestCaptureListModel::taxonScopeAloneSpansEveryTreeSharingTheAncestor()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setTaxonScope(48460);   // "Life" — no project scope
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model),
             (QStringList{QStringLiteral("Caladenia carnea"), QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::projectScopeConfinesTaxonScopeToTheActiveTree()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setProjectScope(m_birdProject);
    QVERIFY(spy.wait(2000));

    spy.clear();
    model.setTaxonScope(48460);   // selecting the "Life" stub in the bird tree
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    spy.clear();
    model.setTaxonScope(900);     // selecting the genus
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    spy.clear();
    model.setTaxonScope(101);     // a taxon not in this tree at all
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.rowCount(), 0);
}

void TestCaptureListModel::projectScopeAloneConfinesToTheActiveTree()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    model.reload();
    QCOMPARE(model.rowCount(), 2);          // no scope at all -> whole library

    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setProjectScope(m_birdProject);   // a tree is active, nothing selected
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    spy.clear();
    model.setProjectScope(0);               // no tree -> back to the whole library
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.rowCount(), 2);
}

void TestCaptureListModel::setScopeCoalescesIntoOneReload()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setScope(m_birdProject, 900);   // one call, both project and taxon scope
    QVERIFY(spy.wait(2000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::rapidSetScopeCallsOnlyApplyTheLatest()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    // Fired back-to-back, as rapid tree clicks/arrow-key moves would --
    // only the second (latest) request's rows must ever be applied.
    model.setScope(m_birdProject, 101);   // a taxon not in this tree: 0 rows
    model.setScope(m_birdProject, 900);   // the genus: 1 row

    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));

    // Give any (incorrectly superseding) stale reply a chance to land, then
    // confirm the result still reflects only the latest request.
    QTest::qWait(200);
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::taxonSetScopeMatchesExactIdsNotASubtreeWalk()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);

    // A set spanning two unrelated branches -- proves it's a literal id match,
    // not a subtree walk that happens to reach both leaves.
    model.setTaxonSetScope(0, {qint64(901), qint64(101)});
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model),
             (QStringList{QStringLiteral("Caladenia carnea"), QStringLiteral("Corvus mellori")}));

    // The genus (900) is an ancestor of the matched species (901) but no
    // capture is matched directly to it -- setTaxonScope(900) reaches 901 via
    // its recursive subtree walk (see taxonScopeAloneSpansEveryTreeSharingTheAncestor),
    // but setTaxonSetScope(900) must not, since it's a literal id set.
    spy.clear();
    model.setTaxonSetScope(0, {qint64(900)});
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.rowCount(), 0);
}

void TestCaptureListModel::taxonSetScopeCombinesWithProjectScope()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);

    // 101 (orchid) isn't part of the bird project's tree, so the project
    // narrowing must exclude it even though it's in the requested id set --
    // this exercises the bind order of the combined project+set clause.
    model.setTaxonSetScope(m_birdProject, {qint64(901), qint64(101)});
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::taxonSetScopeAndTaxonScopeAreMutuallyExclusive()
{
    CaptureListModel model(*m_db, *m_thumbs, this);
    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);

    model.setTaxonSetScope(0, {qint64(101)});
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Caladenia carnea")}));

    // Falling back to a plain subtree scope must drop the stale set filter.
    spy.clear();
    model.setScope(0, 0);
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.rowCount(), 2);   // whole library again, not still confined to {101}
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

void TestCaptureListModel::gpsFilterKeepsOnlyPhotosWithOrWithoutALocation()
{
    // The crow has a location; the orchid hasn't.
    QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false))
        .exec(QStringLiteral("UPDATE capture SET latitude = -37.8, longitude = 144.9 WHERE base_name = 'crow'"));
    CaptureListModel model(*m_db, *m_thumbs, this);
    model.reload();
    QCOMPARE(model.rowCount(), 2);

    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setGpsFilter(CaptureListModel::GpsFilter::With);
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));
    QVERIFY(model.index(0).data(CaptureListModel::HasGpsRole).toBool());

    spy.clear();
    model.setGpsFilter(CaptureListModel::GpsFilter::Without);
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Caladenia carnea")}));

    // Combines with the tree's scope.
    spy.clear();
    model.setProjectScope(m_birdProject);
    QVERIFY(spy.wait(2000));
    QCOMPARE(model.rowCount(), 0);

    spy.clear();
    model.setGpsFilter(CaptureListModel::GpsFilter::Any);
    QVERIFY(spy.wait(2000));
    QCOMPARE(names(model), (QStringList{QStringLiteral("Corvus mellori")}));
}

void TestCaptureListModel::applyGpsPatchesRowInPlaceWithoutReset()
{
    const int captureId = addCaptureMatchedTo(QStringLiteral("crow3"), 901);

    CaptureListModel model(*m_db, *m_thumbs, this);
    model.reload();

    int row = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.index(r).data(CaptureListModel::IdRole).toInt() == captureId) {
            row = r;
            break;
        }
    }
    QVERIFY(row >= 0);
    QVERIFY(!model.index(row).data(CaptureListModel::HasGpsRole).toBool());
    QCOMPARE(model.index(row).data(CaptureListModel::MatchedTaxonInatIdRole).toLongLong(),
             qint64(901));
    QVERIFY(!model.index(row).data(CaptureListModel::PreviewIsRawRole).toBool());

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);

    model.applyGps(captureId, -37.8136, 144.9631);

    QCOMPARE(resetSpy.count(), 0);   // patched in place, not a full reload
    QCOMPARE(dataChangedSpy.count(), 1);

    const QModelIndex idx = model.index(row);
    QVERIFY(idx.data(CaptureListModel::HasGpsRole).toBool());
    QCOMPARE(idx.data(CaptureListModel::LatitudeRole).toDouble(), -37.8136);
    QCOMPARE(idx.data(CaptureListModel::LongitudeRole).toDouble(), 144.9631);

    // A capture id not currently loaded in this model is a harmless no-op.
    dataChangedSpy.clear();
    model.applyGps(999999, 1.0, 2.0);
    QCOMPARE(dataChangedSpy.count(), 0);
}

void TestCaptureListModel::confirmedOrAutoStatusFilterIncludesBothStatuses()
{
    addAutoMatchedCapture(QStringLiteral("crow-auto"), 901);   // another bird-tree capture

    CaptureListModel model(*m_db, *m_thumbs, this);

    QSignalSpy spy(&model, &QAbstractItemModel::modelReset);
    model.setProjectScope(m_birdProject);
    QVERIFY(spy.wait(2000));

    // setStatusFilter() reloads synchronously (unlike setProjectScope()'s
    // background reloadAsync()), so its modelReset has already fired by the
    // time the call returns -- no spy.wait() needed or correct here.
    model.setStatusFilter(QStringLiteral("confirmed"));
    QCOMPARE(model.rowCount(), 1);   // just the original, manually confirmed "crow"

    model.setStatusFilter(QStringLiteral("confirmedOrAuto"));
    QCOMPARE(model.rowCount(), 2);   // "crow" (confirmed) and "crow-auto" (auto) both included
}

QTEST_MAIN(TestCaptureListModel)
#include "tst_capturelistmodel.moc"
