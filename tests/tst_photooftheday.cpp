#include <QtTest>

#include <QFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "catalogue/PhotoOfTheDay.h"
#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using catalogue::PhotoOfTheDay;

class TestPhotoOfTheDay : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void bestShotsComeFirst();
    void theSamePhotoAllDayAndAnotherTomorrow();
    void everyPhotoHasItsDayThenItStartsAgain();
    void onlyIdentifiedPhotosWithAFileQualify();
    void theLineageRunsFromLife();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    void exec(const QString &sql);
    QString conn() const { return m_db->connectionName(); }
};

void TestPhotoOfTheDay::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(conn(), false));
    QVERIFY2(q.exec(sql), qPrintable(sql));
}

void TestPhotoOfTheDay::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(conn());
    const QList<std::tuple<qint64, QString, QString, QString, QString>> taxa{
        {48460, QStringLiteral("stateofmatter"), QStringLiteral("Life"), QString(), QString()},
        {1, QStringLiteral("kingdom"), QStringLiteral("Animalia"), QStringLiteral("Animals"), QStringLiteral("48460")},
        {3, QStringLiteral("class"), QStringLiteral("Aves"), QStringLiteral("Birds"), QStringLiteral("48460/1/2")},
        {12936, QStringLiteral("species"), QStringLiteral("Petroica phoenicea"), QStringLiteral("Flame Robin"),
         QStringLiteral("48460/1/2/3/8000")},
    };
    for (const auto &[id, rank, name, common, ancestry] : taxa) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.rank = rank;
        t.name = name;
        t.commonName = common;
        t.ancestry = ancestry;
        QVERIFY(store.upsertTaxon(t) > 0);
    }
    exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '%1', 'lib', 0)").arg(m_dir->path()));
    // 1-4 identified (1 and 2 best shots); 5 an unreviewed guess; 6 rejected; 7 no file.
    for (int i = 1; i <= 7; ++i) {
        exec(QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on) VALUES (%1, 1, 'IMG_%1', "
                            "'2025-10-18T08:0%1:00')").arg(i));
        const QString path = m_dir->filePath(QStringLiteral("IMG_%1.JPG").arg(i));
        if (i != 7) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        exec(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (%1, '%2', "
                            "'jpeg', 'jpg', 'h%1')").arg(i).arg(path));
        const QString status = i == 5 ? QStringLiteral("pending") : i == 6 ? QStringLiteral("rejected")
                                                                           : QStringLiteral("auto");
        exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                            "SELECT %1, id, 'name', 0.9, '%2', 'engine' FROM taxon WHERE inat_id = 12936")
                 .arg(i).arg(status));
    }
    exec(QStringLiteral("INSERT INTO best_shot (capture_id) VALUES (1), (2)"));
}

void TestPhotoOfTheDay::cleanup()
{
    m_db.reset();
    m_dir.reset();
}

void TestPhotoOfTheDay::bestShotsComeFirst()
{
    const QDate day(2026, 11, 4);
    const auto first = PhotoOfTheDay::choose(conn(), day, {});
    QVERIFY(first);
    QVERIFY(first->bestShot);
    QVERIFY(first->captureId == 1 || first->captureId == 2);
    QCOMPARE(first->commonName, QStringLiteral("Flame Robin"));
    const auto second = PhotoOfTheDay::choose(conn(), day.addDays(1), first->state);
    QVERIFY(second->bestShot);
    QVERIFY(second->captureId != first->captureId);
    const auto third = PhotoOfTheDay::choose(conn(), day.addDays(2), second->state);
    QVERIFY(!third->bestShot);   // both best shots have had their day
}

void TestPhotoOfTheDay::theSamePhotoAllDayAndAnotherTomorrow()
{
    const QDate day(2026, 11, 4);
    const auto today = PhotoOfTheDay::choose(conn(), day, {});
    for (int i = 0; i < 5; ++i)
        QCOMPARE(PhotoOfTheDay::choose(conn(), day, today->state)->captureId, today->captureId);
    const auto tomorrow = PhotoOfTheDay::choose(conn(), day.addDays(1), today->state);
    QVERIFY(tomorrow->captureId != today->captureId);
    QVERIFY(tomorrow->state.shown.contains(today->captureId));
}

void TestPhotoOfTheDay::everyPhotoHasItsDayThenItStartsAgain()
{
    QDate day(2026, 11, 4);
    PhotoOfTheDay::State state;
    QSet<int> seen;
    for (int i = 0; i < 4; ++i) {
        const auto pick = PhotoOfTheDay::choose(conn(), day, state);
        QVERIFY(pick);
        QVERIFY2(!seen.contains(pick->captureId), "no repeats until all have been shown");
        seen.insert(pick->captureId);
        state = pick->state;
        day = day.addDays(1);
    }
    QCOMPARE(seen, (QSet<int>{1, 2, 3, 4}));
    const auto again = PhotoOfTheDay::choose(conn(), day, state);
    QVERIFY(again->bestShot);   // round again, best shots first
}

void TestPhotoOfTheDay::onlyIdentifiedPhotosWithAFileQualify()
{
    // Nothing but 3 is available: never 5 (a guess), 6 (rejected) or 7 (no file).
    const auto pick = PhotoOfTheDay::choose(conn(), QDate(2026, 11, 4), {},
                                            [](const QString &path) { return path.endsWith(QStringLiteral("IMG_3.JPG")); });
    QVERIFY(pick);
    QCOMPARE(pick->captureId, 3);
    QVERIFY(!PhotoOfTheDay::choose(conn(), QDate(2026, 11, 4), {}, [](const QString &) { return false; }));
}

void TestPhotoOfTheDay::theLineageRunsFromLife()
{
    const QList<PhotoOfTheDay::LineageStep> steps = PhotoOfTheDay::lineage(conn(), 12936);
    QList<qint64> ids;
    for (const auto &s : steps)
        ids << s.inatId;
    QCOMPARE(ids, (QList<qint64>{48460, 1, 2, 3, 8000, 12936}));
    QCOMPARE(steps.at(0).name, QStringLiteral("Life"));
    QCOMPARE(steps.at(1).commonName, QStringLiteral("Animals"));
    QVERIFY(!steps.at(2).known());   // not in the catalogue
    QCOMPARE(steps.last().rank, QStringLiteral("species"));
}

QTEST_GUILESS_MAIN(TestPhotoOfTheDay)
#include "tst_photooftheday.moc"
