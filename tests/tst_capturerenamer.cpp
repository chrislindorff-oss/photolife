#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CaptureRenamer.h"
#include "scan/LibraryRenamer.h"

using namespace pl;
using pl::scan::CaptureRenamer;
using pl::scan::LibraryRenamer;

class TestCaptureRenamer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void stemKeepsEverythingButTheName();
    void renamesEveryFileAndTheCatalogue();
    void takenNamesGetTheNextNumber();
    void photosNotAllHereAreLeftAlone();
    void renameToGivesTheWholeName();
    void planNamePartAsTheRenamerWill();
    void planFromPartsCountsPerFolder();
    void planNamesEachPhotoByItsOwnSpecies();
    void folderRenameTakesEverythingWithIt();
    void folderRenameRefusals();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QTemporaryDir> m_dir;
    int m_folder = 0;

    QSqlQuery query() const { return QSqlQuery(QSqlDatabase::database(m_db->connectionName(), false)); }
    void touch(const QString &name)
    {
        QFile f(QDir(m_dir->path()).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(name.toUtf8());
    }
    qint64 addCapture(const QString &base, const QString &nameText, const QStringList &exts, bool files = true)
    {
        QSqlQuery q = query();
        q.prepare(QStringLiteral("INSERT INTO capture (folder_id, base_name, name_text) VALUES (?, ?, ?)"));
        q.addBindValue(m_folder);
        q.addBindValue(base);
        q.addBindValue(nameText);
        if (!q.exec())
            return 0;
        const qint64 id = q.lastInsertId().toLongLong();
        for (const QString &ext : exts) {
            const QString name = base + QLatin1Char('.') + ext;
            if (files)
                touch(name);
            QSqlQuery r = query();
            r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', ?)"));
            r.addBindValue(id);
            r.addBindValue(QDir(m_dir->path()).filePath(name));
            r.addBindValue(ext.toLower());
            r.exec();
        }
        return id;
    }
    bool exists(const QString &name) const { return QFileInfo::exists(QDir(m_dir->path()).filePath(name)); }
};

void TestCaptureRenamer::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_db = std::make_unique<Database>();
    QVERIFY2(m_db->open(QStringLiteral(":memory:")), qPrintable(m_db->error()));
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO folder (path, parent_id, name, depth) VALUES (?, NULL, 'Galls', 0)"));
    q.addBindValue(m_dir->path());
    QVERIFY(q.exec());
    m_folder = q.lastInsertId().toInt();
}

void TestCaptureRenamer::cleanup()
{
    m_db.reset();
    m_dir.reset();
}

void TestCaptureRenamer::stemKeepsEverythingButTheName()
{
    QCOMPARE(CaptureRenamer::renamedStem(QStringLiteral("Wasp Gall on Red Box 1 - Sailors Tk, Steiglitz 14-6-2020 (1)"),
                                         QStringLiteral("Wasp Gall on Red Box 1"), QStringLiteral("Apiomorpha")),
             QStringLiteral("Apiomorpha - Sailors Tk, Steiglitz 14-6-2020 (1)"));
    // Organ tags stay with the rest.
    QCOMPARE(CaptureRenamer::renamedStem(QStringLiteral("Diuris pardina (bud) - Dadswells Bridge 28-9-2020"),
                                         QStringLiteral("Diuris pardina"), QStringLiteral("Leopard Orchid")),
             QStringLiteral("Leopard Orchid (bud) - Dadswells Bridge 28-9-2020"));
    // A name read differently from how it's written: from the " - " on.
    QCOMPARE(CaptureRenamer::renamedStem(QStringLiteral("Petroica_boodang - Anakie 1-1-2021"),
                                         QStringLiteral("Petroica boodang"), QStringLiteral("Scarlet Robin")),
             QStringLiteral("Scarlet Robin - Anakie 1-1-2021"));
    // Nothing but a name: all of it.
    QCOMPARE(CaptureRenamer::renamedStem(QStringLiteral("DSC_1234"), QStringLiteral("DSC_1234"),
                                         QStringLiteral("Scarlet Robin")),
             QStringLiteral("Scarlet Robin"));
    QCOMPARE(CaptureRenamer::renamedStem(QStringLiteral("x"), QStringLiteral("x"), QStringLiteral("A/B: C?")),
             QStringLiteral("A B C"));
}

void TestCaptureRenamer::renamesEveryFileAndTheCatalogue()
{
    const QString base = QStringLiteral("Wasp Gall 1 - Sailors Tk 14-6-2020 (1)");
    const qint64 id = addCapture(base, QStringLiteral("Wasp Gall 1"), {QStringLiteral("NEF"), QStringLiteral("JPG")});
    touch(base + QStringLiteral(".NEF.xmp"));
    touch(base + QStringLiteral(".notes.txt"));   // not a sidecar: left alone

    const CaptureRenamer::Result r = CaptureRenamer(m_db->connectionName()).rename(id, QStringLiteral("Apiomorpha"));
    QVERIFY2(r.renamed, qPrintable(r.error));
    const QString now = QStringLiteral("Apiomorpha - Sailors Tk 14-6-2020 (1)");
    QCOMPARE(r.newBaseName, now);
    QVERIFY(exists(now + QStringLiteral(".NEF")));
    QVERIFY(exists(now + QStringLiteral(".JPG")));
    QVERIFY(exists(now + QStringLiteral(".NEF.xmp")));
    QVERIFY(!exists(base + QStringLiteral(".NEF")));
    QVERIFY(exists(base + QStringLiteral(".notes.txt")));

    QSqlQuery q = query();
    QVERIFY(q.exec(QStringLiteral("SELECT base_name, name_text, locality_text FROM capture WHERE id = %1").arg(id)));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), now);
    QCOMPARE(q.value(1).toString(), QStringLiteral("Apiomorpha"));
    QCOMPARE(q.value(2).toString(), QStringLiteral("Sailors Tk"));
    QVERIFY(q.exec(QStringLiteral("SELECT path FROM rendition WHERE capture_id = %1").arg(id)));
    while (q.next())
        QVERIFY2(QFileInfo(q.value(0).toString()).fileName().startsWith(now), qPrintable(q.value(0).toString()));
}

void TestCaptureRenamer::takenNamesGetTheNextNumber()
{
    addCapture(QStringLiteral("Apiomorpha - X 1-1-2020 (1)"), QStringLiteral("Apiomorpha"), {QStringLiteral("JPG")});
    const qint64 a = addCapture(QStringLiteral("Gall - X 1-1-2020 (1)"), QStringLiteral("Gall"), {QStringLiteral("NEF")});
    const qint64 b = addCapture(QStringLiteral("Gall - Y 2-1-2020"), QStringLiteral("Gall"), {QStringLiteral("JPG")});
    touch(QStringLiteral("Apiomorpha - Y 2-1-2020.txt"));   // any extension takes the name

    CaptureRenamer renamer(m_db->connectionName());
    QCOMPARE(renamer.rename(a, QStringLiteral("Apiomorpha")).newBaseName, QStringLiteral("Apiomorpha - X 1-1-2020 (2)"));
    QCOMPARE(renamer.rename(b, QStringLiteral("Apiomorpha")).newBaseName, QStringLiteral("Apiomorpha - Y 2-1-2020 (2)"));
    QVERIFY(exists(QStringLiteral("Apiomorpha - X 1-1-2020 (1).JPG")));   // never overwritten
    QVERIFY(exists(QStringLiteral("Apiomorpha - X 1-1-2020 (2).NEF")));
}

void TestCaptureRenamer::photosNotAllHereAreLeftAlone()
{
    const qint64 id = addCapture(QStringLiteral("Gall 1"), QStringLiteral("Gall"), {QStringLiteral("JPG")});
    // A second rendition kept online only: not on this computer.
    QSqlQuery r = query();
    r.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'raw', 'nef')"));
    r.addBindValue(id);
    r.addBindValue(QDir(m_dir->path()).filePath(QStringLiteral("Gall 1.NEF")));
    QVERIFY(r.exec());

    const CaptureRenamer::Result result = CaptureRenamer(m_db->connectionName()).rename(id, QStringLiteral("Apiomorpha"));
    QVERIFY(!result.renamed);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(exists(QStringLiteral("Gall 1.JPG")));
    QVERIFY(!exists(QStringLiteral("Apiomorpha.JPG")));
}

void TestCaptureRenamer::renameToGivesTheWholeName()
{
    const qint64 id = addCapture(QStringLiteral("DSC_0001"), QStringLiteral("DSC_0001"),
                                 {QStringLiteral("NEF"), QStringLiteral("JPG")});
    touch(QStringLiteral("Petroica phoenicea_2020-01-01.txt"));
    CaptureRenamer renamer(m_db->connectionName());
    const CaptureRenamer::Result r = renamer.renameTo(id, QStringLiteral("Petroica phoenicea_2020-01-01"));
    QVERIFY2(r.renamed, qPrintable(r.error));
    QCOMPARE(r.newBaseName, QStringLiteral("Petroica phoenicea_2020-01-01 (2)"));   // the .txt has the name
    QVERIFY(exists(QStringLiteral("Petroica phoenicea_2020-01-01 (2).NEF")));
    // And back again.
    QVERIFY(renamer.renameTo(id, QStringLiteral("DSC_0001")).renamed);
    QVERIFY(exists(QStringLiteral("DSC_0001.JPG")));
}

void TestCaptureRenamer::planNamePartAsTheRenamerWill()
{
    const qint64 a = addCapture(QStringLiteral("Gall - X 1-1-2020 (1)"), QStringLiteral("Gall"), {QStringLiteral("JPG")});
    const qint64 b = addCapture(QStringLiteral("Wasp Gall - X 1-1-2020 (1)"), QStringLiteral("Wasp Gall"),
                                {QStringLiteral("JPG")});
    const qint64 c = addCapture(QStringLiteral("Apiomorpha - Y"), QStringLiteral("Apiomorpha"), {QStringLiteral("JPG")});
    const qint64 away = addCapture(QStringLiteral("Gone"), QStringLiteral("Gone"), {QStringLiteral("JPG")}, false);

    QList<LibraryRenamer::Photo> photos = LibraryRenamer::load(m_db->connectionName(), {a, b, c, away});
    QCOMPARE(photos.size(), 4);
    LibraryRenamer::plan(photos, LibraryRenamer::Mode::NamePart, QStringLiteral("Apiomorpha"), {}, {});
    QHash<qint64, QString> planned;
    for (const auto &p : photos)
        planned.insert(p.captureId, p.newStem);
    QCOMPARE(planned.value(c), QStringLiteral("Apiomorpha - Y"));        // no change
    QCOMPARE(planned.value(away), QStringLiteral("Gone"));               // not here: left
    // The first of the two to be renamed takes the name; the second the next "(n)".
    const QSet<QString> both{planned.value(a), planned.value(b)};
    QCOMPARE(both, (QSet<QString>{QStringLiteral("Apiomorpha - X 1-1-2020 (1)"),
                                  QStringLiteral("Apiomorpha - X 1-1-2020 (2)")}));

    // Renaming in the planned order gives exactly those names.
    CaptureRenamer renamer(m_db->connectionName());
    for (const auto &p : std::as_const(photos)) {
        if (p.newStem != p.oldStem)
            QCOMPARE(renamer.renameTo(p.captureId, p.newStem).newBaseName, p.newStem);
    }
}

void TestCaptureRenamer::planFromPartsCountsPerFolder()
{
    const qint64 a = addCapture(QStringLiteral("IMG_2"), QStringLiteral("IMG_2"), {QStringLiteral("JPG")});
    const qint64 b = addCapture(QStringLiteral("IMG_1"), QStringLiteral("IMG_1"), {QStringLiteral("JPG")});
    QSqlQuery q = query();
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2020-01-01T10:00:00', locality_text = "
                                  "'Anakie' WHERE id = %1").arg(b)));
    QVERIFY(q.exec(QStringLiteral("UPDATE capture SET captured_on = '2020-01-01T11:00:00', locality_text = "
                                  "'Anakie' WHERE id = %1").arg(a)));
    collection::FileNamePattern pattern;
    pattern.parts = {{collection::NameToken::Text, QStringLiteral("Robin")},
                     {collection::NameToken::FileLocality, {}},
                     {collection::NameToken::DateTaken, {}},
                     {collection::NameToken::Sequence, {}}};
    pattern.separator = QStringLiteral(" ");
    QList<LibraryRenamer::Photo> photos = LibraryRenamer::load(m_db->connectionName(), {a, b});
    LibraryRenamer::plan(photos, LibraryRenamer::Mode::Pattern, {}, pattern, {});
    QCOMPARE(photos.at(0).captureId, b);   // taken first
    QCOMPARE(photos.at(0).newStem, QStringLiteral("Robin Anakie 2020-01-01 001"));
    QCOMPARE(photos.at(1).newStem, QStringLiteral("Robin Anakie 2020-01-01 002"));
}

void TestCaptureRenamer::planNamesEachPhotoByItsOwnSpecies()
{
    using Form = LibraryRenamer::NameForm;
    QCOMPARE(LibraryRenamer::speciesName(QStringLiteral("Petroica phoenicea"), QStringLiteral("Flame Robin"),
                                         Form::CommonScientific),
             QStringLiteral("Flame Robin (Petroica phoenicea)"));
    QCOMPARE(LibraryRenamer::speciesName(QStringLiteral("Petroica phoenicea"), QStringLiteral("Flame Robin"),
                                         Form::ScientificCommon),
             QStringLiteral("Petroica phoenicea (Flame Robin)"));
    // No common name: the scientific one, in any form.
    QCOMPARE(LibraryRenamer::speciesName(QStringLiteral("Apiomorpha"), QString(), Form::Common),
             QStringLiteral("Apiomorpha"));
    QCOMPARE(LibraryRenamer::speciesName(QStringLiteral("Apiomorpha"), QStringLiteral("Apiomorpha"),
                                         Form::CommonScientific),
             QStringLiteral("Apiomorpha"));
    QVERIFY(LibraryRenamer::speciesName(QString(), QStringLiteral("x"), Form::Common).isEmpty());

    const qint64 robin = addCapture(QStringLiteral("Robin - Anakie 1-1-2021"), QStringLiteral("Robin"),
                                    {QStringLiteral("JPG")});
    const qint64 teal = addCapture(QStringLiteral("Duck - Lara 2-1-2021"), QStringLiteral("Duck"), {QStringLiteral("JPG")});
    const qint64 unknown = addCapture(QStringLiteral("Bird - Lara 3-1-2021"), QStringLiteral("Bird"),
                                      {QStringLiteral("JPG")});
    QSqlQuery q = query();
    QVERIFY(q.exec(QStringLiteral("INSERT INTO taxon (inat_id, name, rank, common_name) VALUES "
                                  "(12936, 'Petroica phoenicea', 'species', 'Flame Robin'), "
                                  "(7000, 'Anas castanea', 'species', 'Chestnut Teal')")));
    for (const auto &[capture, inatId] : {std::pair{robin, 12936}, std::pair{teal, 7000}}) {
        QVERIFY(q.exec(QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, "
                                      "decided_by) SELECT %1, id, 'name', 0.9, 'confirmed', 'user' FROM taxon "
                                      "WHERE inat_id = %2").arg(capture).arg(inatId)));
    }
    QList<LibraryRenamer::Photo> photos = LibraryRenamer::load(m_db->connectionName(), {robin, teal, unknown});
    LibraryRenamer::plan(photos, LibraryRenamer::Mode::NamePart, {}, {}, {}, Form::ScientificCommon);
    QHash<qint64, QString> planned;
    for (const auto &p : std::as_const(photos))
        planned.insert(p.captureId, p.newStem);
    QCOMPARE(planned.value(robin), QStringLiteral("Petroica phoenicea (Flame Robin) - Anakie 1-1-2021"));
    QCOMPARE(planned.value(teal), QStringLiteral("Anas castanea (Chestnut Teal) - Lara 2-1-2021"));
    QCOMPARE(planned.value(unknown), QStringLiteral("Bird - Lara 3-1-2021"));   // no species: kept
}

void TestCaptureRenamer::folderRenameTakesEverythingWithIt()
{
    // The folder renamed lives inside the temporary directory.
    QDir top(m_dir->path());
    QVERIFY(top.mkpath(QStringLiteral("Robin/Anakie")));
    const QString robin = top.filePath(QStringLiteral("Robin"));
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, 'Robin', 1)"));
    q.addBindValue(robin);
    q.addBindValue(m_folder);
    QVERIFY(q.exec());
    const int folder = q.lastInsertId().toInt();
    q.prepare(QStringLiteral("INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, 'Anakie', 2)"));
    q.addBindValue(robin + QStringLiteral("/Anakie"));
    q.addBindValue(folder);
    QVERIFY(q.exec());
    const int inner = q.lastInsertId().toInt();
    m_folder = inner;
    const QString inside = QStringLiteral("Robin/Anakie/R1.JPG");
    touch(inside);
    QVERIFY(q.exec(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (%1, 'R1')").arg(inner)));
    const qint64 capture = q.lastInsertId().toLongLong();
    q.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    q.addBindValue(capture);
    q.addBindValue(top.filePath(inside));
    QVERIFY(q.exec());
    // A neighbour whose name starts the same is no part of it.
    QVERIFY(top.mkpath(QStringLiteral("Robins")));
    q.prepare(QStringLiteral("INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, 'Robins', 1)"));
    q.addBindValue(top.filePath(QStringLiteral("Robins")));
    q.addBindValue(m_folder);
    QVERIFY(q.exec());

    const LibraryRenamer::FolderResult r = LibraryRenamer::renameFolder(
        m_db->connectionName(), folder, QStringLiteral("Petroica phoenicea"), {m_dir->path()});
    QVERIFY2(r.renamed, qPrintable(r.error));
    const QString now = top.filePath(QStringLiteral("Petroica phoenicea"));
    QCOMPARE(r.newPath, now);
    QVERIFY(QFileInfo::exists(now + QStringLiteral("/Anakie/R1.JPG")));
    QVERIFY(!QFileInfo::exists(robin));
    QVERIFY(q.exec(QStringLiteral("SELECT path, name FROM folder WHERE id = %1").arg(folder)));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), now);
    QCOMPARE(q.value(1).toString(), QStringLiteral("Petroica phoenicea"));
    QVERIFY(q.exec(QStringLiteral("SELECT path FROM folder WHERE id = %1").arg(inner)));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), now + QStringLiteral("/Anakie"));
    QVERIFY(q.exec(QStringLiteral("SELECT path FROM rendition WHERE capture_id = %1").arg(capture)));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), now + QStringLiteral("/Anakie/R1.JPG"));
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM folder WHERE path = '%1'").arg(top.filePath(QStringLiteral("Robins")))));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
}

void TestCaptureRenamer::folderRenameRefusals()
{
    QDir top(m_dir->path());
    QVERIFY(top.mkpath(QStringLiteral("A")));
    QVERIFY(top.mkpath(QStringLiteral("B")));
    QSqlQuery q = query();
    q.prepare(QStringLiteral("INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, 'A', 1)"));
    q.addBindValue(top.filePath(QStringLiteral("A")));
    q.addBindValue(m_folder);
    QVERIFY(q.exec());
    const int a = q.lastInsertId().toInt();
    const QString conn = m_db->connectionName();

    // A library root, or a folder holding one.
    QVERIFY(!LibraryRenamer::renameFolder(conn, m_folder, QStringLiteral("Other"), {m_dir->path()}).renamed);
    QVERIFY(!LibraryRenamer::renameFolder(conn, a, QStringLiteral("Z"), {top.filePath(QStringLiteral("A"))}).renamed);
    // A name already taken.
    QVERIFY(!LibraryRenamer::renameFolder(conn, a, QStringLiteral("B"), {m_dir->path()}).renamed);
    // A photo in it kept online only.
    QVERIFY(q.exec(QStringLiteral("INSERT INTO capture (folder_id, base_name) VALUES (%1, 'X')").arg(a)));
    const qint64 capture = q.lastInsertId().toLongLong();
    q.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext) VALUES (?, ?, 'jpeg', 'jpg')"));
    q.addBindValue(capture);
    q.addBindValue(top.filePath(QStringLiteral("A/X.JPG")));
    QVERIFY(q.exec());
    const LibraryRenamer::FolderResult r = LibraryRenamer::renameFolder(conn, a, QStringLiteral("Z"), {m_dir->path()});
    QVERIFY(!r.renamed);
    QVERIFY(!r.error.isEmpty());
    QVERIFY(QFileInfo::exists(top.filePath(QStringLiteral("A"))));
    QVERIFY(!QFileInfo::exists(top.filePath(QStringLiteral("Z"))));
}

QTEST_GUILESS_MAIN(TestCaptureRenamer)
#include "tst_capturerenamer.moc"
