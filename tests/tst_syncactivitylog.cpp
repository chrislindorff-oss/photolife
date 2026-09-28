#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "sync/SyncActivityLog.h"

using namespace pl::sync;

class TestSyncActivityLog : public QObject
{
    Q_OBJECT

private slots:
    void appendsAndReadsNewestFirst();
    void skipsDamagedLines();
};

void TestSyncActivityLog::appendsAndReadsNewestFirst()
{
    QTemporaryDir dir;
    const SyncActivityLog log(dir.filePath(QStringLiteral("sync-activity.log")));
    QVERIFY(log.read().isEmpty());   // no file yet

    const QDateTime t0 = QDateTime::fromString(QStringLiteral("2026-09-28T10:00:00.000"), Qt::ISODateWithMs);
    QVERIFY(log.append({t0, QStringLiteral("Start Editing"), true, QStringLiteral("Got version 3"), {}, 1200}));
    QVERIFY(log.append({t0.addSecs(60), QStringLiteral("Sync"), false, {}, QStringLiteral("server gone"), 0}));

    const auto entries = log.read();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).action, QStringLiteral("Sync"));
    QVERIFY(!entries.at(0).ok);
    QCOMPARE(entries.at(0).error, QStringLiteral("server gone"));
    QCOMPARE(entries.at(1).detail, QStringLiteral("Got version 3"));
    QCOMPARE(entries.at(1).durationMs, qint64(1200));
    QCOMPARE(entries.at(1).at, t0);
    QCOMPARE(log.read(1).size(), 1);
}

void TestSyncActivityLog::skipsDamagedLines()
{
    QTemporaryDir dir;
    const SyncActivityLog log(dir.filePath(QStringLiteral("sync-activity.log")));
    log.append({QDateTime::currentDateTime(), QStringLiteral("Sync"), true, {}, {}, 0});
    {
        QFile f(log.path());
        QVERIFY(f.open(QIODevice::Append));
        f.write("{not json\n");
    }
    log.append({QDateTime::currentDateTime(), QStringLiteral("Finish Editing"), true, {}, {}, 0});
    QCOMPARE(log.read().size(), 2);
}

QTEST_GUILESS_MAIN(TestSyncActivityLog)
#include "tst_syncactivitylog.moc"
