#include <QtTest>

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "db/Database.h"
#include "geo/LocalityStore.h"
#include "inat/ExifWriter.h"
#include "organise/OrganiseMover.h"
#include "organise/OrganiseSession.h"

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
    void localityLookupUsesTheSameRounding();
    void runsRejectsAndMergedNames();
    void namingCanEndAndResume();
    void progressIsKeptAndRecognisesTheShots();
    void organiseAndUndo();
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
