#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "catalogue/CameraInfoStore.h"
#include "coverage/LifeList.h"
#include "model/LifeListModel.h"
#include "thumb/ThumbnailCache.h"
#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::coverage;

namespace {

const QDate kToday(2026, 11, 6);

LifeListNode node(qint64 id, qint64 parent, const char *rank, const char *name, const char *common = "")
{
    return {id, parent, QString::fromLatin1(rank), QString::fromLatin1(name), QString::fromLatin1(common)};
}

LifeListSighting seen(qint64 taxon, int capture, const char *on)
{
    return {taxon, capture, QString::fromLatin1(on)};
}

QList<qint64> idsOf(const QList<LifeListEntry> &entries)
{
    QList<qint64> ids;
    for (const auto &e : entries)
        ids << e.inatId;
    return ids;
}

QList<LifeListNode> tree()
{
    return {
        node(1, 0, "genus", "Petroica"),
        node(10, 1, "species", "Petroica boodang", "Scarlet Robin"),
        node(11, 10, "subspecies", "Petroica boodang leggii"),
        node(20, 1, "species", "Petroica phoenicea", "Flame Robin"),
        node(30, 1, "species", "Petroica rosea", "Rose Robin"),
        node(40, 1, "hybrid", "Petroica × hybrida"),
    };
}

} // namespace

class TestLifeList : public QObject
{
    Q_OBJECT

private slots:
    void numbersSpeciesByTheirFirstPhoto();
    void sameDayTiesGoByTimeThenCapture();
    void subspeciesPhotosDateTheirSpecies();
    void belowSpeciesAreEntriesOnlyWhenAskedFor();
    void genusOnlyPhotosAreIgnored();
    void unusableDatesAreIgnored();
    void countsEachPhotoOnce();
    void loadsFromTheCatalogue();
    void loadsCameraDetailsOnceRead();
    void modelShowsCameraAndFocal();
};

void TestLifeList::numbersSpeciesByTheirFirstPhoto()
{
    const auto all = buildLifeList(tree(),
                                   {seen(20, 1, "2015-06-01"), seen(10, 2, "2012-03-14"), seen(20, 3, "2011-01-02"),
                                    seen(30, 4, "2020-12-25")},
                                   kToday);
    const auto shown = numberedLifeList(all, false);
    QCOMPARE(idsOf(shown), (QList<qint64>{20, 10, 30}));
    QCOMPARE(shown[0].number, 1);
    QCOMPARE(shown[0].firstOn, QStringLiteral("2011-01-02"));
    QCOMPARE(shown[0].captureId, 3);
    QCOMPARE(shown[0].commonName, QStringLiteral("Flame Robin"));
    QCOMPARE(shown[2].number, 3);
}

void TestLifeList::sameDayTiesGoByTimeThenCapture()
{
    const auto shown = numberedLifeList(
        buildLifeList(tree(),
                      {seen(20, 9, "2012-03-14T10:00:00"), seen(10, 8, "2012-03-14T08:30:00"),
                       seen(30, 7, "2012-03-14T10:00:00")},
                      kToday),
        false);
    QCOMPARE(idsOf(shown), (QList<qint64>{10, 30, 20}));
}

void TestLifeList::subspeciesPhotosDateTheirSpecies()
{
    const auto shown =
        numberedLifeList(buildLifeList(tree(), {seen(10, 1, "2018-01-01"), seen(11, 2, "2010-05-05")}, kToday),
                         false);
    QCOMPARE(shown.size(), 1);
    QCOMPARE(shown[0].inatId, qint64(10));
    QCOMPARE(shown[0].firstOn, QStringLiteral("2010-05-05"));
    QCOMPARE(shown[0].captureId, 2);
    QCOMPARE(shown[0].photoCount, 2);
}

void TestLifeList::belowSpeciesAreEntriesOnlyWhenAskedFor()
{
    const auto all = buildLifeList(
        tree(), {seen(11, 1, "2010-05-05"), seen(40, 2, "2009-01-01"), seen(20, 3, "2011-01-01")}, kToday);
    // The hybrid has no species above it: only an entry of its own.
    QCOMPARE(idsOf(numberedLifeList(all, false)), (QList<qint64>{10, 20}));
    const auto withBelow = numberedLifeList(all, true);
    // The subspecies and its species share a first photo: the species first.
    QCOMPARE(idsOf(withBelow), (QList<qint64>{40, 10, 11, 20}));
    QCOMPARE(withBelow[2].number, 3);
    QVERIFY(withBelow[2].belowSpecies);
    QVERIFY(!withBelow[1].belowSpecies);
}

void TestLifeList::genusOnlyPhotosAreIgnored()
{
    const auto all = buildLifeList(tree(), {seen(1, 1, "2001-01-01"), seen(99, 2, "2001-01-01")}, kToday);
    QVERIFY(all.isEmpty());
}

void TestLifeList::unusableDatesAreIgnored()
{
    const auto shown = numberedLifeList(
        buildLifeList(tree(),
                      {seen(10, 1, "2912-09-29"), seen(10, 2, "2019-04-04"), seen(20, 3, "2912-09-29"),
                       seen(30, 4, ""), seen(20, 5, "")},
                      kToday),
        false);
    QCOMPARE(idsOf(shown), (QList<qint64>{10, 20, 30}));
    QCOMPARE(shown[0].number, 1);
    QCOMPARE(shown[0].firstOn, QStringLiteral("2019-04-04"));
    QCOMPARE(shown[1].number, 0);
    QVERIFY(shown[1].firstOn.isEmpty());
    QCOMPARE(shown[1].captureId, 3);   // a photo of it, for the thumbnail
    QCOMPARE(shown[1].photoCount, 2);
    QCOMPARE(shown[2].number, 0);
}

void TestLifeList::countsEachPhotoOnce()
{
    // One photo matched to both the species and its subspecies.
    const auto shown =
        numberedLifeList(buildLifeList(tree(), {seen(10, 1, "2018-01-01"), seen(11, 1, "2018-01-01")}, kToday),
                         true);
    QCOMPARE(shown.size(), 2);
    QCOMPARE(shown[0].photoCount, 1);
}

void TestLifeList::loadsFromTheCatalogue()
{
    Database database;
    QVERIFY(database.open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(database.connectionName());
    const int project = store.ensureProject(QStringLiteral("P"), 1, std::nullopt, QStringLiteral("inat"));
    for (const LifeListNode &n : tree()) {
        taxonomy::Taxon t;
        t.inatId = n.inatId;
        if (n.parentInatId > 0)
            t.parentInatId = n.parentInatId;
        t.rank = n.rank;
        t.name = n.name;
        t.commonName = n.commonName;
        store.upsertTaxon(t);
        store.addProjectTaxon(project, n.inatId, true, false);
    }
    QSqlDatabase db = QSqlDatabase::database(database.connectionName(), false);
    QSqlQuery f(db);
    QVERIFY(f.exec(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES ('/photos', 'photos', 0)")));
    const int folder = f.lastInsertId().toInt();
    const auto capture = [&](const char *name, const char *on, qint64 taxon, const char *status) {
        QSqlQuery c(db);
        c.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name, captured_on, locality_text) "
                                 "VALUES (?, ?, ?, 'Steiglitz')"));
        c.addBindValue(folder);
        c.addBindValue(QString::fromLatin1(name));
        c.addBindValue(QString::fromLatin1(on));
        c.exec();
        const int id = c.lastInsertId().toInt();
        QSqlQuery r(db);
        r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash, file_size, mtime) "
                                 "VALUES (?, ?, 'jpeg', 'jpg', ?, 1, 1)"));
        r.addBindValue(id);
        r.addBindValue(QStringLiteral("/photos/%1.jpg").arg(QString::fromLatin1(name)));
        r.addBindValue(QStringLiteral("hash-%1").arg(QString::fromLatin1(name)));
        if (!r.exec())
            qWarning() << r.lastError().text();
        QSqlQuery m(db);
        m.prepare(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                                 "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'file', 1.0, ?)"));
        m.addBindValue(id);
        m.addBindValue(taxon);
        m.addBindValue(QString::fromLatin1(status));
        m.exec();
        return id;
    };
    const int flame = capture("flame", "2014-02-02", 20, "auto");
    capture("scarlet", "2016-02-02", 10, "confirmed");
    capture("rose", "2001-01-01", 30, "pending");   // not a match yet

    const auto shown = numberedLifeList(loadLifeList(database.connectionName(), project, kToday), false);
    QCOMPARE(idsOf(shown), (QList<qint64>{20, 10}));
    QCOMPARE(shown[0].captureId, flame);
    QCOMPARE(shown[0].previewPath, QStringLiteral("/photos/flame.jpg"));
    QCOMPARE(shown[0].previewHash, QStringLiteral("hash-flame"));
    QCOMPARE(shown[0].locality, QStringLiteral("Steiglitz"));
}

void TestLifeList::loadsCameraDetailsOnceRead()
{
    Database database;
    QVERIFY(database.open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(database.connectionName());
    const int project = store.ensureProject(QStringLiteral("P"), 1, std::nullopt, QStringLiteral("inat"));
    for (const LifeListNode &n : tree()) {
        taxonomy::Taxon t;
        t.inatId = n.inatId;
        if (n.parentInatId > 0)
            t.parentInatId = n.parentInatId;
        t.rank = n.rank;
        t.name = n.name;
        store.upsertTaxon(t);
        store.addProjectTaxon(project, n.inatId, true, false);
    }
    QSqlDatabase db = QSqlDatabase::database(database.connectionName(), false);
    QSqlQuery f(db);
    QVERIFY(f.exec(QStringLiteral("INSERT INTO folder (path, name, depth) VALUES ('/p', 'p', 0)")));
    const int folder = f.lastInsertId().toInt();
    QList<int> ids;
    for (const auto &[name, taxon] : {std::pair{"flame", 20}, std::pair{"rose", 30}}) {
        QSqlQuery c(db);
        c.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name, captured_on) VALUES (?, ?, '2015-01-01')"));
        c.addBindValue(folder);
        c.addBindValue(QString::fromLatin1(name));
        QVERIFY(c.exec());
        const int id = c.lastInsertId().toInt();
        ids << id;
        QSqlQuery r(db);
        r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash, file_size, mtime) "
                                 "VALUES (?, ?, 'jpeg', 'jpg', 'h', 1, 1)"));
        r.addBindValue(id);
        r.addBindValue(QStringLiteral("/p/%1.jpg").arg(QString::fromLatin1(name)));
        QVERIFY(r.exec());
        QSqlQuery m(db);
        m.prepare(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status) "
                                 "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), 'file', 1.0, 'auto')"));
        m.addBindValue(id);
        m.addBindValue(taxon);
        QVERIFY(m.exec());
    }
    catalogue::CameraInfoStore cameras(database.connectionName());
    QVERIFY(cameras.save({{ids[0], QStringLiteral("Nikon D7000"), 300.0}}));
    QVERIFY(cameras.save({{ids[0], QStringLiteral("Nikon D7000"), 400.0}}));   // read again: replaced

    const auto shown = numberedLifeList(loadLifeList(database.connectionName(), project, kToday), false);
    QCOMPARE(shown.size(), 2);
    for (const LifeListEntry &e : shown) {
        if (e.captureId == ids[0]) {
            QVERIFY(e.cameraChecked);
            QCOMPARE(e.camera, QStringLiteral("Nikon D7000"));
            QCOMPARE(e.focalLength.value_or(0), 400.0);
        } else {
            QVERIFY(!e.cameraChecked);   // not read yet
            QVERIFY(e.camera.isEmpty());
        }
    }
}

void TestLifeList::modelShowsCameraAndFocal()
{
    QTemporaryDir dir;
    thumb::ThumbnailCache thumbs(dir.path());
    model::LifeListModel model(thumbs);
    LifeListEntry a;
    a.number = 1;
    a.captureId = 7;
    a.name = QStringLiteral("Petroica phoenicea");
    a.previewPath = QStringLiteral("/p/a.jpg");
    LifeListEntry b = a;
    b.number = 2;
    b.captureId = 8;
    model.setEntries({a, b});
    const QModelIndex camera = model.index(0, model::LifeListModel::CameraColumn);
    const QModelIndex focal = model.index(0, model::LifeListModel::FocalColumn);
    QVERIFY(camera.data().toString().isEmpty());
    QVERIFY(camera.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Reading")));

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setCameraInfo(7, QStringLiteral("Nikon D70"), 4.53);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(camera.data().toString(), QStringLiteral("Nikon D70"));
    QCOMPARE(focal.data().toString(), QStringLiteral("4.5 mm"));
    QVERIFY(model.index(1, model::LifeListModel::CameraColumn).data().toString().isEmpty());
    // Unknown sorts after every camera; focal by number.
    QVERIFY(model.index(0, model::LifeListModel::CameraColumn).data(model::LifeListModel::SortRole).toString()
            < model.index(1, model::LifeListModel::CameraColumn).data(model::LifeListModel::SortRole).toString());
    QCOMPARE(focal.data(model::LifeListModel::SortRole).toDouble(), 4.53);
}

QTEST_MAIN(TestLifeList)
#include "tst_lifelist.moc"
