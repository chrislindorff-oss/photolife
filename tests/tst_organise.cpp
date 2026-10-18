#include <QtTest>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

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
    void runsRejectsAndMergedNames();
    void progressIsKeptAndRecognisesTheShots();
    void organiseAndUndo();
    void neverOverwrites();
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
        QVERIFY(s.saveProgress());
    }
    OrganiseSession again;
    again.setItems(tmp.path(), OrganiseSession::scanFolder(tmp.path()));
    again.loadProgress();
    QCOMPARE(again.nameAt(0), QStringLiteral("Flame Robin"));
    QVERIFY(again.isRejected(1));

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

QTEST_GUILESS_MAIN(TestOrganise)
#include "tst_organise.moc"
