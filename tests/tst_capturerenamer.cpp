#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "scan/CaptureRenamer.h"

using namespace pl;
using pl::scan::CaptureRenamer;

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

QTEST_GUILESS_MAIN(TestCaptureRenamer)
#include "tst_capturerenamer.moc"
