#include <QtTest>

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "geo/LocalityStore.h"
#include "inat/ExifWriter.h"
#include "organise/OrganiseMover.h"
#include "scan/CatalogueWriter.h"
#include "scan/Exif.h"
#include "scan/FileScanner.h"
#include "organise/OrganiseSession.h"

using namespace pl;
using namespace pl::organise;

namespace {

// A tiny real JPEG (so it's recognised and has a size), dated by mtime.
void writeShot(const QDir &dir, const QString &name, qint64 secs)
{
    const QString path = dir.filePath(name);
    if (name.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive)) {
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path, "JPG"));
    } else {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("raw-or-sidecar " + name.toUtf8());
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
}

QStringList baseNames(const QList<Item> &items)
{
    QStringList out;
    for (const Item &i : items)
        out << i.baseName;
    return out;
}

QList<ShotMove> movesFor(const OrganiseSession &s)
{
    QList<ShotMove> moves;
    const OrganisePlan plan = s.plan();
    for (const FolderMove &f : plan.folders) {
        for (int i : f.items)
            moves.append({s.items().at(i).baseName, s.items().at(i).files, f.folderName});
    }
    for (int i : plan.rejects)
        moves.append({s.items().at(i).baseName, s.items().at(i).files,
                      QLatin1String(OrganiseSession::kRejectsFolder)});
    return moves;
}

} // namespace

class TestOrganise : public QObject
{
    Q_OBJECT

private slots:
    void scanGroupsPairsAndSidecarsInShootingOrder();
    void scanReportsPhotoProgress();
    void scanReadsGps();
    void bigFoldersLoadAPageAtATime();
    void localityLookupUsesTheSameRounding();
    void runsRejectsAndMergedNames();
    void namingCanEndAndResume();
    void progressIsKeptAndRecognisesTheShots();
    void organiseAndUndo();
    void renamedBaseNames();
    void bestShotsAreKeptAndExcludeRejects();
    void bestShotsAreRatedFiveStars();
    void fiveStarPhotosArriveAsBestShots();
    void locationIsWrittenAndKeptWithTheShot();
    void organiseRenamesShotsTogetherAndUndoPutsNamesBack();
    void neverOverwrites();
    void undoTakesBackOneWholeOrganiseAtATime();
};

void TestOrganise::scanGroupsPairsAndSidecarsInShootingOrder()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    // The camera's counter rolled over: DSC_0001 was taken after DSC_9999.
    writeShot(dir, QStringLiteral("DSC_9999.NEF"), 1000);
    writeShot(dir, QStringLiteral("DSC_9999.JPG"), 1000);
    writeShot(dir, QStringLiteral("DSC_9999.NEF.xmp"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.NEF"), 2000);
    writeShot(dir, QStringLiteral("DSC_0001.xmp"), 2000);
    writeShot(dir, QStringLiteral("DSC_0002.JPG"), 2000);   // same time: name decides
    writeShot(dir, QStringLiteral("notes.txt"), 500);
    QDir().mkpath(dir.filePath(QStringLiteral("Flame Robin")));
    writeShot(QDir(dir.filePath(QStringLiteral("Flame Robin"))), QStringLiteral("DSC_0100.JPG"), 10);

    const QList<Item> items = OrganiseSession::scanFolder(tmp.path());
    QCOMPARE(baseNames(items), (QStringList{QStringLiteral("DSC_9999"), QStringLiteral("DSC_0001"),
                                            QStringLiteral("DSC_0002")}));
    const Item &first = items.first();
    QCOMPARE(first.files.size(), 3);
    QVERIFY(first.files.first().endsWith(QLatin1String("DSC_9999.NEF")));   // RAW first
    QVERIFY(first.files.last().endsWith(QLatin1String("DSC_9999.NEF.xmp")));
    QVERIFY(first.previewPath.endsWith(QLatin1String("DSC_9999.JPG")));
    QCOMPARE(first.kinds, (QStringList{QStringLiteral("NEF"), QStringLiteral("JPG")}));
    QCOMPARE(items.at(1).files.size(), 2);   // NEF + its xmp
    QVERIFY(items.at(1).previewPath.endsWith(QLatin1String("DSC_0001.NEF")));
}

void TestOrganise::scanReportsPhotoProgress()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("A.JPG"), 1000);
    writeShot(dir, QStringLiteral("B.NEF"), 2000);
    QDir().mkpath(dir.filePath(QStringLiteral("Already Organised")));
    writeShot(QDir(dir.filePath(QStringLiteral("Already Organised"))), QStringLiteral("C.JPG"), 3000);

    QList<QPair<int, int>> progress;
    const QList<Item> items = OrganiseSession::scanFolder(
        tmp.path(), [&progress](int done, int total) { progress.append({done, total}); });

    QCOMPARE(items.size(), 2);
    QCOMPARE(progress, (QList<QPair<int, int>>{{0, 0}, {0, 2}, {1, 2}, {2, 2}}));
}

void TestOrganise::scanReadsGps()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.jpg"), 1000);
    writeShot(dir, QStringLiteral("DSC_0002.jpg"), 2000);
    pl::inat::ExifFields fields;
    fields.latitude = -35.27512;
    fields.longitude = 149.12843;
    QString error;
    QVERIFY2(pl::inat::writeExif(dir.filePath(QStringLiteral("DSC_0001.jpg")), fields, &error),
             qPrintable(error));

    QList<Item> items = OrganiseSession::scanFolder(tmp.path());
    QCOMPARE(items.size(), 2);
    // (Writing the EXIF changed its file time, so sort by name.)
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.baseName < b.baseName; });
    QVERIFY(items.at(0).hasGps());
    QVERIFY(std::abs(*items.at(0).latitude - -35.27512) < 1e-4);
    QVERIFY(std::abs(*items.at(0).longitude - 149.12843) < 1e-4);
    QVERIFY(!items.at(1).hasGps());
}

void TestOrganise::bigFoldersLoadAPageAtATime()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    const int total = OrganiseSession::kPageSize + 60;
    for (int i = 0; i < total; ++i)
        writeShot(dir, QStringLiteral("DSC_%1.NEF").arg(i, 4, 10, QLatin1Char('0')), 1000 + i);

    // Listed in file-time order, quickly: nothing read yet.
    QList<Item> listed = OrganiseSession::listFolder(tmp.path());
    QCOMPARE(listed.size(), total);
    QCOMPARE(listed.first().baseName, QStringLiteral("DSC_0000"));
    QCOMPARE(listed.last().baseName, QStringLiteral("DSC_%1").arg(total - 1, 4, 10, QLatin1Char('0')));
    QCOMPARE(listed.at(5).taken, QDateTime::fromSecsSinceEpoch(1005));
    // No progress: one page first.
    QCOMPARE(OrganiseSession::firstPageSize(tmp.path(), listed), OrganiseSession::kPageSize);

    // Name a photo beyond the first page, and one in it.
    const int beyond = OrganiseSession::kPageSize + 20;
    {
        OrganiseSession all;
        all.setItems(tmp.path(), listed);
        all.setName(3, QStringLiteral("Flame Robin"));
        all.setName(beyond, QStringLiteral("Scarlet Robin"));
        QVERIFY(all.saveProgress());
    }
    // Carrying on reads far enough to reach it (and a page past, here the lot).
    QCOMPARE(OrganiseSession::firstPageSize(tmp.path(), listed), total);

    // A page at a time: the second page arrives with its saved names.
    OrganiseSession s;
    QList<Item> first = listed.mid(0, OrganiseSession::kPageSize);
    s.setItems(tmp.path(), first, listed.mid(OrganiseSession::kPageSize));
    s.loadProgress();
    QCOMPARE(s.count(), OrganiseSession::kPageSize);
    QCOMPARE(s.pendingCount(), 60);
    QCOMPARE(s.nameAt(3), QStringLiteral("Flame Robin"));
    // Flame Robin runs to the end of what's loaded.
    QCOMPARE(s.runName(OrganiseSession::kPageSize - 1), QStringLiteral("Flame Robin"));

    // Saving with only the first page loaded keeps the later photo's name.
    s.setRejected(10, true);
    QVERIFY(s.saveProgress());

    QList<Item> next = s.pending().mid(0, 30);
    for (Item &item : next)
        OrganiseSession::readMetadata(item);
    QCOMPARE(s.appendItems(next), 30);
    QCOMPARE(s.count(), OrganiseSession::kPageSize + 30);
    QCOMPARE(s.pendingCount(), 30);
    QCOMPARE(s.items().at(beyond).baseName, listed.at(beyond).baseName);
    QCOMPARE(s.nameAt(beyond), QStringLiteral("Scarlet Robin"));
    QCOMPARE(s.runName(beyond - 1), QStringLiteral("Flame Robin"));
    QVERIFY(s.isRejected(10));
}

void TestOrganise::localityLookupUsesTheSameRounding()
{
    QTemporaryDir tmp;
    pl::Database db;
    QVERIFY(db.open(QDir(tmp.path()).filePath(QStringLiteral("catalogue.sqlite"))));
    pl::geo::LocalityStore store(db.connectionName());
    QVERIFY(!store.lookup(-35.275, 149.128).has_value());
    QVERIFY(store.upsertLocality(-35.27512, 149.12843, QStringLiteral("Canberra, ACT, Australia")));
    // Within the same ~100 m square: found; further away: not.
    QCOMPARE(store.lookup(-35.2749, 149.1281).value_or(QString()), QStringLiteral("Canberra, ACT, Australia"));
    QVERIFY(!store.lookup(-35.29, 149.13).has_value());
    // "Nothing there" is a result too.
    QVERIFY(store.upsertLocality(-40.0, 150.0, QString()));
    QCOMPARE(store.lookup(-40.0, 150.0).value_or(QStringLiteral("missing")), QString());

    // A place cached with no town before the locality keys were read is
    // looked up once more; one cached since is kept.
    QVERIFY(store.upsertLocality(-25.954, 153.105, QStringLiteral("Queensland, Australia")));
    QCOMPARE(store.lookup(-25.954, 153.105).value_or(QString()), QStringLiteral("Queensland, Australia"));
    QSqlQuery q(QSqlDatabase::database(db.connectionName()));
    QVERIFY(q.exec(QStringLiteral("UPDATE geocode_cache SET fetched_at = '2026-10-20T08:00:00.000Z'")));
    QVERIFY(!store.lookup(-25.954, 153.105).has_value());
    QCOMPARE(store.lookup(-35.2749, 149.1281).value_or(QString()), QStringLiteral("Canberra, ACT, Australia"));
    QCOMPARE(store.lookup(-40.0, 150.0).value_or(QStringLiteral("missing")), QString());
    QVERIFY(store.upsertLocality(-25.954, 153.105, QStringLiteral("Cooloola, Queensland, Australia")));
    QCOMPARE(store.lookup(-25.954, 153.105).value_or(QString()), QStringLiteral("Cooloola, Queensland, Australia"));
}

void TestOrganise::runsRejectsAndMergedNames()
{
    OrganiseSession s;
    QList<Item> items;
    for (int i = 0; i < 8; ++i)
        items << Item{QStringLiteral("P%1").arg(i), {}, {}, {}, {}, false, 0, 0};
    s.setItems(QStringLiteral("/tmp/day"), items);
    s.setName(2, QStringLiteral("Flame Robin"));
    s.setName(4, QStringLiteral("Superb Fairywren"));
    s.setName(6, QStringLiteral("flame robin"));   // same taxon again later
    s.setRejected(3, true);
    s.setRejected(0, true);

    QCOMPARE(s.runStart(1), -1);
    QCOMPARE(s.runName(3), QStringLiteral("Flame Robin"));
    QCOMPARE(s.runName(7), QStringLiteral("flame robin"));
    QCOMPARE(s.runNumber(5), 1);

    const OrganisePlan plan = s.plan();
    QCOMPARE(plan.unnamed, 1);   // P1 (P0 is rejected)
    QCOMPARE(plan.rejects, (QList<int>{0, 3}));
    QCOMPARE(plan.folders.size(), 2);
    QCOMPARE(plan.folders.at(0).folderName, QStringLiteral("Flame Robin"));
    QCOMPARE(plan.folders.at(0).items, (QList<int>{2, 6, 7}));
    QCOMPARE(plan.folders.at(1).items, (QList<int>{4, 5}));
    QCOMPARE(plan.photos(), 5);
    QCOMPARE(s.recentNames().first(), QStringLiteral("flame robin"));

    s.setName(4, QString());   // cleared: its photos join the run before
    QCOMPARE(s.runName(5), QStringLiteral("Flame Robin"));
    QCOMPARE(OrganiseSession::folderNameFor(QStringLiteral("Robin: Flame?.")), QStringLiteral("Robin Flame"));
}

void TestOrganise::namingCanEndAndResume()
{
    OrganiseSession s;
    QList<Item> items;
    for (int i = 0; i < 5; ++i)
        items << Item{QStringLiteral("P%1").arg(i), {}, {}, {}, {}, false, 0, 0};
    s.setItems(QStringLiteral("/tmp/day"), items);
    s.setName(0, QStringLiteral("Flame Robin"));
    s.setName(4, QStringLiteral("Superb Fairywren"));
    s.setNamingEndAt(2, true);

    QCOMPARE(s.runName(1), QStringLiteral("Flame Robin"));
    QCOMPARE(s.runName(2), QStringLiteral("Flame Robin"));
    QVERIFY(s.runName(3).isEmpty());
    QCOMPARE(s.runName(4), QStringLiteral("Superb Fairywren"));
    QCOMPARE(s.plan().folders.at(0).items, (QList<int>{0, 1, 2}));
    QCOMPARE(s.plan().unnamed, 1);

    s.setNamingEndAt(2, false);
    QCOMPARE(s.runName(3), QStringLiteral("Flame Robin"));
}

void TestOrganise::progressIsKeptAndRecognisesTheShots()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("A.JPG"), 1000);
    writeShot(dir, QStringLiteral("B.JPG"), 2000);
    {
        OrganiseSession s;
        s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
        s.setName(0, QStringLiteral("Flame Robin"));
        s.setRejected(1, true);
        s.setNamingEndAt(1, true);
        QVERIFY(s.saveProgress());
    }
    OrganiseSession again;
    again.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    again.loadProgress();
    QCOMPARE(again.nameAt(0), QStringLiteral("Flame Robin"));
    QVERIFY(again.isRejected(1));
    QVERIFY(again.isNamingEndAt(1));

    // A file replaced since isn't taken for the same shot.
    writeShot(dir, QStringLiteral("A.JPG"), 5000);
    OrganiseSession changed;
    changed.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    changed.loadProgress();
    QVERIFY(changed.nameAt(1).isEmpty() && changed.nameAt(0).isEmpty());
}

void TestOrganise::organiseAndUndo()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.NEF"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.JPG"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.NEF.xmp"), 1000);
    writeShot(dir, QStringLiteral("DSC_0002.NEF"), 2000);
    writeShot(dir, QStringLiteral("DSC_0003.NEF"), 3000);
    writeShot(dir, QStringLiteral("DSC_0004.NEF"), 4000);

    OrganiseSession s;
    s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    s.setName(1, QStringLiteral("Flame Robin"));
    s.setRejected(3, true);
    const OrganiseMover::Result r = OrganiseMover::execute(tmp.path(), movesFor(s));
    QVERIFY2(r.failures.isEmpty(), qPrintable(r.failures.join(QLatin1Char('\n'))));
    QCOMPARE(r.moved, 3);
    QVERIFY(dir.exists(QStringLiteral("DSC_0001.NEF")) && dir.exists(QStringLiteral("DSC_0001.NEF.xmp")));   // unnamed: stays
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0002.NEF")));
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0003.NEF")));
    QVERIFY(dir.exists(QStringLiteral("Rejects/DSC_0004.NEF")));
    QVERIFY(OrganiseMover::canUndo(tmp.path()));

    const OrganiseMover::Result back = OrganiseMover::undo(tmp.path());
    QVERIFY(back.failures.isEmpty());
    QCOMPARE(back.moved, 3);
    for (const char *f : {"DSC_0002.NEF", "DSC_0003.NEF", "DSC_0004.NEF"})
        QVERIFY(dir.exists(QLatin1String(f)));
    QVERIFY(!dir.exists(QStringLiteral("Flame Robin")));   // made by it, empty again
    QVERIFY(!dir.exists(QStringLiteral("Rejects")));
    QVERIFY(!OrganiseMover::canUndo(tmp.path()));
}

void TestOrganise::bestShotsAreKeptAndExcludeRejects()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.jpg"), 1000);
    writeShot(dir, QStringLiteral("DSC_0002.jpg"), 2000);
    OrganiseSession s;
    s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    s.setRejected(0, true);
    s.setBest(0, true);   // a best shot isn't a reject
    QVERIFY(s.isBest(0) && !s.isRejected(0));
    s.setBest(1, true);
    s.setRejected(1, true);   // ...and a reject isn't a best shot
    QVERIFY(!s.isBest(1) && s.isRejected(1));
    QCOMPARE(s.bestShots(), (QList<int>{0}));
    QVERIFY(s.saveProgress());

    OrganiseSession again;
    again.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    again.loadProgress();
    QVERIFY(again.isBest(0));
    QVERIFY(!again.isBest(1));
}

void TestOrganise::bestShotsAreRatedFiveStars()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.jpg"), 1000);   // JPEG only
    writeShot(dir, QStringLiteral("DSC_0002.NEF"), 2000);   // RAW + JPEG
    writeShot(dir, QStringLiteral("DSC_0002.jpg"), 2000);
    writeShot(dir, QStringLiteral("DSC_0003.NEF"), 3000);   // RAW with darktable's sidecar
    QVERIFY(inat::writeSidecarRating(dir.filePath(QStringLiteral("DSC_0003.NEF.xmp")), 1));
    const QByteArray raw = [&] {
        QFile f(dir.filePath(QStringLiteral("DSC_0002.NEF")));
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }();

    const QList<Item> items = OrganiseSession::scanFolder(tmp.path());
    QCOMPARE(items.size(), 3);
    QString created, error;
    QVERIFY2(OrganiseSession::rateFiveStars(items.at(0), &created, &error), qPrintable(error));
    QVERIFY(created.isEmpty());   // the JPEG holds it
    QCOMPARE(scan::readXmpRating(dir.filePath(QStringLiteral("DSC_0001.jpg"))).value_or(0), 5);
    QCOMPARE(QFileInfo(dir.filePath(QStringLiteral("DSC_0001.jpg"))).lastModified().toSecsSinceEpoch(), 1000);

    QVERIFY2(OrganiseSession::rateFiveStars(items.at(1), &created, &error), qPrintable(error));
    QCOMPARE(QFileInfo(created).fileName(), QStringLiteral("DSC_0002.xmp"));
    QCOMPARE(scan::readXmpRating(created).value_or(0), 5);
    QCOMPARE(scan::readXmpRating(dir.filePath(QStringLiteral("DSC_0002.jpg"))).value_or(0), 5);
    QFile f(dir.filePath(QStringLiteral("DSC_0002.NEF")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), raw);   // the RAW untouched

    created.clear();
    QVERIFY2(OrganiseSession::rateFiveStars(items.at(2), &created, &error), qPrintable(error));
    QVERIFY(created.isEmpty());   // its own sidecar, updated
    QCOMPARE(scan::readXmpRating(dir.filePath(QStringLiteral("DSC_0003.NEF.xmp"))).value_or(0), 5);
}

void TestOrganise::fiveStarPhotosArriveAsBestShots()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("Robin 1.jpg"), 1000);
    writeShot(dir, QStringLiteral("Robin 2.jpg"), 2000);
    writeShot(dir, QStringLiteral("Robin 3.NEF"), 3000);
    inat::ExifFields five;
    five.rating = 5;
    QVERIFY(inat::writeExif(dir.filePath(QStringLiteral("Robin 1.jpg")), five));
    inat::ExifFields three;
    three.rating = 3;
    QVERIFY(inat::writeExif(dir.filePath(QStringLiteral("Robin 2.jpg")), three));
    QVERIFY(inat::writeSidecarRating(dir.filePath(QStringLiteral("Robin 3.xmp")), 5));

    Database db;
    QVERIFY(db.open(dir.filePath(QStringLiteral("catalogue.sqlite"))));
    scan::CatalogueWriter writer(db.connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({tmp.path()}), {tmp.path()}).ok());
    QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("SELECT c.base_name FROM best_shot b JOIN capture c ON c.id = b.capture_id "
                                  "ORDER BY c.base_name")));
    QStringList best;
    while (q.next())
        best << q.value(0).toString();
    QCOMPARE(best, (QStringList{QStringLiteral("Robin 1"), QStringLiteral("Robin 3")}));
}

void TestOrganise::locationIsWrittenAndKeptWithTheShot()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.jpg"), 1000);   // a JPEG
    writeShot(dir, QStringLiteral("DSC_0002.NEF"), 2000);   // RAW + JPEG
    writeShot(dir, QStringLiteral("DSC_0002.jpg"), 2000);
    writeShot(dir, QStringLiteral("DSC_0003.NEF"), 3000);   // a RAW alone
    OrganiseSession s;
    s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    QCOMPARE(s.count(), 3);
    s.setName(0, QStringLiteral("Bar-shouldered Dove"));
    s.setRejected(2, true);
    QVERIFY(s.saveProgress());
    for (int i = 0; i < 3; ++i) {
        QVERIFY(!s.items().at(i).hasGps());
        QVERIFY(s.setLocation(i, -25.954, 153.105, false).ok());
        QVERIFY(s.items().at(i).hasGps());
        QVERIFY(std::abs(*s.items().at(i).latitude - -25.954) < 1e-5);
    }
    // The JPEGs carry it themselves; the lone RAW in a new sidecar, which
    // now goes with the shot.
    QVERIFY(scan::readJpegExif(dir.filePath(QStringLiteral("DSC_0002.jpg"))).hasGps());
    QVERIFY(s.items().at(2).files.contains(dir.filePath(QStringLiteral("DSC_0003.xmp"))));
    QCOMPARE(s.items().at(1).files.size(), 3);   // NEF, JPEG and its new sidecar

    // Opened again: every shot has its location, and is still recognised.
    OrganiseSession again;
    again.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    again.loadProgress();
    QCOMPARE(again.count(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY2(again.items().at(i).hasGps(), qPrintable(again.items().at(i).baseName));
    QCOMPARE(again.nameAt(0), QStringLiteral("Bar-shouldered Dove"));
    QVERIFY(again.isRejected(2));

    // Added to the library, the lone RAW brings its sidecar's location.
    Database db;
    QVERIFY(db.open(dir.filePath(QStringLiteral("catalogue.sqlite"))));
    scan::CatalogueWriter writer(db.connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({tmp.path()}), {tmp.path()}).ok());
    QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("SELECT latitude, longitude FROM capture WHERE base_name = 'DSC_0003'")));
    QVERIFY(q.next());
    QVERIFY(std::abs(q.value(0).toDouble() - -25.954) < 1e-5);
    QVERIFY(std::abs(q.value(1).toDouble() - 153.105) < 1e-5);
}

void TestOrganise::renamedBaseNames()
{
    const QDateTime taken(QDate(2025, 10, 18), QTime(9, 30));
    QCOMPARE(OrganiseSession::renamedBase(QStringLiteral("Scarlet Robin"), QStringLiteral("Anakie, Victoria"), taken),
             QStringLiteral("Scarlet Robin - Anakie, Victoria, 2025-10-18"));
    QCOMPARE(OrganiseSession::renamedBase(QStringLiteral("Scarlet Robin"), QString(), taken),
             QStringLiteral("Scarlet Robin - 2025-10-18"));
    // Nothing a file system can't take.
    QCOMPARE(OrganiseSession::renamedBase(QStringLiteral("Robin?"), QStringLiteral("Mt. A/B: \"C\""), taken),
             QStringLiteral("Robin - Mt. A B C, 2025-10-18"));
    // Long names are cut to fit, keeping the place and date.
    const QString longName = OrganiseSession::renamedBase(QString(300, QLatin1Char('x')), QStringLiteral("Anakie"), taken);
    QVERIFY(longName.size() <= 200);
    QVERIFY(longName.endsWith(QStringLiteral(" - Anakie, 2025-10-18")));
}

void TestOrganise::organiseRenamesShotsTogetherAndUndoPutsNamesBack()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    writeShot(dir, QStringLiteral("DSC_0001.NEF"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.JPG"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.NEF.xmp"), 1000);
    writeShot(dir, QStringLiteral("DSC_0002.NEF"), 2000);   // RAW only
    writeShot(dir, QStringLiteral("DSC_0003.JPG"), 3000);   // JPEG only
    writeShot(dir, QStringLiteral("DSC_0004.NEF"), 4000);

    OrganiseSession s;
    s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    QCOMPARE(s.count(), 4);
    s.setName(0, QStringLiteral("Scarlet Robin"));
    s.setRejected(3, true);
    QList<ShotMove> moves = movesFor(s);
    const QString base = QStringLiteral("Scarlet Robin - Anakie, Victoria, 2025-10-18");
    for (ShotMove &m : moves) {
        if (m.folderName == QLatin1String("Scarlet Robin"))
            m.newBaseName = base;
    }
    const OrganiseMover::Result r = OrganiseMover::execute(tmp.path(), moves);
    QVERIFY2(r.failures.isEmpty(), qPrintable(r.failures.join(QLatin1Char('\n'))));
    QCOMPARE(r.moved, 4);
    // One shot's files share one name; the next shots are numbered in order,
    // never sharing a number though their extensions differ.
    const QDir robin(dir.filePath(QStringLiteral("Scarlet Robin")));
    QVERIFY(robin.exists(base + QStringLiteral(".NEF")));
    QVERIFY(robin.exists(base + QStringLiteral(".JPG")));
    QVERIFY(robin.exists(base + QStringLiteral(".NEF.xmp")));
    QVERIFY(robin.exists(base + QStringLiteral(" (2).NEF")));
    QVERIFY(robin.exists(base + QStringLiteral(" (3).JPG")));
    QVERIFY(!robin.exists(base + QStringLiteral(" (2).JPG")));
    QVERIFY(dir.exists(QStringLiteral("Rejects/DSC_0004.NEF")));   // rejects keep their names

    const OrganiseMover::Result back = OrganiseMover::undo(tmp.path());
    QVERIFY(back.failures.isEmpty());
    for (const char *f : {"DSC_0001.NEF", "DSC_0001.JPG", "DSC_0001.NEF.xmp", "DSC_0002.NEF", "DSC_0003.JPG",
                          "DSC_0004.NEF"})
        QVERIFY2(dir.exists(QLatin1String(f)), f);
    QVERIFY(!dir.exists(QStringLiteral("Scarlet Robin")));
}

void TestOrganise::neverOverwrites()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    QDir().mkpath(dir.filePath(QStringLiteral("Flame Robin")));
    writeShot(QDir(dir.filePath(QStringLiteral("Flame Robin"))), QStringLiteral("DSC_0001.JPG"), 10);
    writeShot(dir, QStringLiteral("DSC_0001.NEF"), 1000);
    writeShot(dir, QStringLiteral("DSC_0001.JPG"), 1000);

    OrganiseSession s;
    s.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    s.setName(0, QStringLiteral("Flame Robin"));
    const OrganiseMover::Result r = OrganiseMover::execute(tmp.path(), movesFor(s));
    QVERIFY(r.failures.isEmpty());
    // The JPEG's name was taken, so the whole shot became "DSC_0001 (2)".
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0001 (2).NEF")));
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0001 (2).JPG")));
    QFile kept(dir.filePath(QStringLiteral("Flame Robin/DSC_0001.JPG")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(QDateTime(kept.fileTime(QFileDevice::FileModificationTime)).toSecsSinceEpoch(), qint64(10));
    QVERIFY(OrganiseMover::undo(tmp.path()).failures.isEmpty());
    QVERIFY(dir.exists(QStringLiteral("DSC_0001.NEF")));
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0001.JPG")));   // the folder wasn't ours: kept
}

// Undo is a whole organise -- every folder of it -- and each one in turn,
// most recent first; the preview says exactly what goes back.
void TestOrganise::undoTakesBackOneWholeOrganiseAtATime()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    for (int i = 1; i <= 6; ++i)
        writeShot(dir, QStringLiteral("DSC_000%1.NEF").arg(i), 1000 * i);
    writeShot(dir, QStringLiteral("DSC_0002.JPG"), 2000);   // a pair: one photo

    OrganiseSession first;
    first.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    first.setName(0, QStringLiteral("Flame Robin"));
    first.setName(2, QStringLiteral("Willie Wagtail"));
    first.setName(4, QString());   // nothing more named: 0-3 move, 4-5 stay
    QList<ShotMove> moves = movesFor(first);
    moves.erase(std::remove_if(moves.begin(), moves.end(), [](const ShotMove &m) {
                    return m.baseName == QLatin1String("DSC_0005") || m.baseName == QLatin1String("DSC_0006");
                }),
                moves.end());
    QCOMPARE(OrganiseMover::execute(tmp.path(), moves).moved, 4);

    OrganiseSession second;
    second.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    QCOMPARE(second.count(), 2);
    second.setName(0, QStringLiteral("Superb Fairywren"));
    QCOMPARE(OrganiseMover::execute(tmp.path(), movesFor(second)).moved, 2);

    // Most recent first: just the fairywrens.
    OrganiseMover::UndoPreview preview = OrganiseMover::previewUndo(tmp.path());
    QCOMPARE(preview.folders.size(), 1);
    QCOMPARE(preview.folders.first().name, QStringLiteral("Superb Fairywren"));
    QCOMPARE(preview.shots, 2);
    QCOMPARE(preview.earlier, 1);
    QVERIFY(preview.when.isValid());
    QCOMPARE(OrganiseMover::undo(tmp.path()).moved, 2);
    QVERIFY(dir.exists(QStringLiteral("DSC_0005.NEF")));
    QVERIFY(dir.exists(QStringLiteral("Flame Robin/DSC_0001.NEF")));   // the first organise stands

    // Then the first one, both folders of it.
    preview = OrganiseMover::previewUndo(tmp.path());
    QCOMPARE(preview.folders.size(), 2);
    QCOMPARE(preview.folders.at(0).name, QStringLiteral("Flame Robin"));
    QCOMPARE(preview.folders.at(0).shots, 2);
    QCOMPARE(preview.folders.at(0).files, 3);   // DSC_0002 is NEF + JPG
    QCOMPARE(preview.folders.at(1).name, QStringLiteral("Willie Wagtail"));
    QCOMPARE(preview.earlier, 0);

    // A file moved away since is counted, and left alone.
    QVERIFY(QFile::rename(dir.filePath(QStringLiteral("Willie Wagtail/DSC_0004.NEF")),
                          dir.filePath(QStringLiteral("elsewhere.NEF"))));
    QCOMPARE(OrganiseMover::previewUndo(tmp.path()).missing, 1);
    const OrganiseMover::Result r = OrganiseMover::undo(tmp.path());
    QCOMPARE(r.moved, 3);
    QCOMPARE(r.failures.size(), 1);
    QVERIFY(dir.exists(QStringLiteral("DSC_0002.JPG")));
    QVERIFY(dir.exists(QStringLiteral("elsewhere.NEF")));
    QVERIFY(!dir.exists(QStringLiteral("Flame Robin")));
    QVERIFY(!OrganiseMover::canUndo(tmp.path()));
}

QTEST_GUILESS_MAIN(TestOrganise)
#include "tst_organise.moc"
