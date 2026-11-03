#include <QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "inat/InatDuplicateCheck.h"
#include "inat/ObservationUploader.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/UploadPhotoPicker.h"

using namespace pl;

class TestUploadPhotoPicker : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void filtersNarrowTheList();
    void photosAreLoadedWithWhatTheCatalogueKnows();
    void aFullFolderPathMatchesThatFolder();
    void photosNotInTheLibraryAreCounted();
    void copiesOfPhotosOnINaturalistAreKnown();
    void theLibraryKnowsObservationsByDay();

private:
    std::unique_ptr<Database> m_db;

    QList<int> found(const UploadPhotoPicker::Filter &filter) const;
};

void TestUploadPhotoPicker::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(m_db->connectionName());
    taxonomy::Taxon robin;
    robin.inatId = 12936;
    robin.rank = QStringLiteral("species");
    robin.name = QStringLiteral("Petroica phoenicea");
    robin.commonName = QStringLiteral("Flame Robin");
    QVERIFY(store.upsertTaxon(robin) > 0);
    taxonomy::Taxon wren = robin;
    wren.inatId = 7000;
    wren.name = QStringLiteral("Malurus cyaneus");
    wren.commonName = QStringLiteral("Superb Fairywren");
    QVERIFY(store.upsertTaxon(wren) > 0);

    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    const QStringList setup{
        QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/lib/Birds/2025 Lakeside', '2025 Lakeside', 2)"),
        QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (2, '/lib/Orchids', 'Orchids', 1)"),
        // 1: robin, dated with a time, GPS. 2: wren, a date only. 3: on iNaturalist already.
        // 4: no file. 5: in Orchids, no species.
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on, latitude, longitude) "
                       "VALUES (1, 1, 'IMG_1', '2025-10-18T08:15:30', -37.83, 144.98)"),
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on) VALUES (2, 1, 'IMG_2', '2025-11-02')"),
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on, inat_observation_id, inat_photo_id) "
                       "VALUES (3, 1, 'IMG_3', '2025-10-19T09:00:00', 901, 4401)"),
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name, captured_on) VALUES (4, 1, 'IMG_4', '2025-10-20')"),
        QStringLiteral("INSERT INTO capture (id, folder_id, base_name) VALUES (5, 2, 'IMG_5')"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (1, '/lib/Birds/2025 Lakeside/IMG_1.CR3', 'raw', 'cr3', 'h1raw')"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (1, '/lib/Birds/2025 Lakeside/IMG_1.JPG', 'jpeg', 'jpg', 'h1')"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (2, '/lib/Birds/2025 Lakeside/IMG_2.JPG', 'jpeg', 'jpg', 'h2')"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (3, '/lib/Birds/2025 Lakeside/IMG_3.JPG', 'jpeg', 'jpg', 'h3')"),
        QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (5, '/lib/Orchids/IMG_5.JPG', 'jpeg', 'jpg', 'h5')"),
        // 1: an engine match the user rejected, and their confirmed one.
        QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                       "SELECT 1, id, 'name', 0.99, 'rejected', 'user' FROM taxon WHERE inat_id = 7000"),
        QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                       "SELECT 1, id, 'name', 0.5, 'confirmed', 'user' FROM taxon WHERE inat_id = 12936"),
        QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                       "SELECT 2, id, 'name', 0.9, 'auto', 'engine' FROM taxon WHERE inat_id = 7000"),
        QStringLiteral("INSERT INTO capture_match (capture_id, taxon_id, method, confidence, status, decided_by) "
                       "SELECT 3, id, 'name', 0.9, 'auto', 'engine' FROM taxon WHERE inat_id = 12936"),
    };
    for (const QString &sql : setup)
        QVERIFY2(q.exec(sql), qPrintable(sql));
}

void TestUploadPhotoPicker::cleanup()
{
    m_db.reset();
}

QList<int> TestUploadPhotoPicker::found(const UploadPhotoPicker::Filter &filter) const
{
    QList<int> ids;
    for (const UploadPhotoPicker::Found &f : UploadPhotoPicker::find(m_db->connectionName(), filter))
        ids << f.captureId;
    return ids;
}

void TestUploadPhotoPicker::filtersNarrowTheList()
{
    UploadPhotoPicker::Filter any;
    any.notOnInat = false;
    QCOMPARE(found(any), (QList<int>{2, 3, 1, 5}));   // newest first, undated last; 4 has no file

    UploadPhotoPicker::Filter fresh;
    QCOMPARE(found(fresh), (QList<int>{2, 1, 5}));

    UploadPhotoPicker::Filter dates;
    dates.from = QDate(2025, 10, 1);
    dates.to = QDate(2025, 10, 31);
    QCOMPARE(found(dates), (QList<int>{1}));

    UploadPhotoPicker::Filter folder;
    folder.folder = QStringLiteral("orchid");
    QCOMPARE(found(folder), (QList<int>{5}));

    UploadPhotoPicker::Filter species;
    species.species = QStringLiteral("robin");   // its common name
    QCOMPARE(found(species), (QList<int>{1}));
    species.species = QStringLiteral("Malurus");
    QCOMPARE(found(species), (QList<int>{2}));

    UploadPhotoPicker::Filter literal;
    literal.folder = QStringLiteral("%");
    QVERIFY(found(literal).isEmpty());

    const QList<UploadPhotoPicker::Found> one = UploadPhotoPicker::find(m_db->connectionName(), dates);
    QCOMPARE(one.at(0).path, QStringLiteral("/lib/Birds/2025 Lakeside/IMG_1.JPG"));   // the JPEG, not the RAW
    QCOMPARE(one.at(0).species, QStringLiteral("Flame Robin"));
}

void TestUploadPhotoPicker::photosAreLoadedWithWhatTheCatalogueKnows()
{
    QList<inat::UploadPhoto> photos = inat::ObservationUploader::loadPhotos(m_db->connectionName(), {1, 2, 3, 99});
    QCOMPARE(photos.size(), 3);
    std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) { return a.captureId < b.captureId; });
    const inat::UploadPhoto &one = photos.at(0);
    QCOMPARE(one.taxon.inatId, 12936);   // the confirmed match, not the rejected one
    QCOMPARE(one.taxon.commonName, QStringLiteral("Flame Robin"));
    QCOMPARE(one.path, QStringLiteral("/lib/Birds/2025 Lakeside/IMG_1.JPG"));
    QCOMPARE(one.contentHash, QStringLiteral("h1"));
    QCOMPARE(one.label, QStringLiteral("IMG_1.JPG"));
    QCOMPARE(one.taken, QDateTime(QDate(2025, 10, 18), QTime(8, 15, 30)));
    QVERIFY(one.hasTime);
    QCOMPARE(*one.latitude, -37.83);
    const inat::UploadPhoto &two = photos.at(1);
    QVERIFY(!two.hasTime);
    QCOMPARE(two.taken.date(), QDate(2025, 11, 2));
    QVERIFY(!two.latitude);
    QCOMPARE(two.taxon.inatId, 7000);
    QCOMPARE(photos.at(2).observationId, 901);
    QCOMPARE(photos.at(2).photoId, 4401);
}

void TestUploadPhotoPicker::aFullFolderPathMatchesThatFolder()
{
    UploadPhotoPicker::Filter birds;
    birds.folder = QStringLiteral("/lib/Birds");
    QCOMPARE(found(birds), (QList<int>{2, 1}));   // in its subfolder
    birds.subfolders = false;
    QVERIFY(found(birds).isEmpty());
    birds.folder = QStringLiteral("/lib/Birds/2025 Lakeside");
    QCOMPARE(found(birds), (QList<int>{2, 1}));
    UploadPhotoPicker::Filter partial;
    partial.folder = QStringLiteral("/lib/Bird");   // a full path is a folder, not part of a name
    QVERIFY(found(partial).isEmpty());
}

void TestUploadPhotoPicker::photosNotInTheLibraryAreCounted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
    for (const QString &name : {QStringLiteral("a.jpg"), QStringLiteral("b.CR3"), QStringLiteral("notes.txt"),
                                QStringLiteral("sub/c.jpg")}) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) VALUES (5, ?, 'jpeg', 'jpg', 'ha')"));
    q.addBindValue(dir.filePath(QStringLiteral("a.jpg")));
    QVERIFY(q.exec());
    QCOMPARE(UploadPhotoPicker::countUncatalogued(m_db->connectionName(), dir.path(), true), 2);   // b.CR3, sub/c.jpg
    QCOMPARE(UploadPhotoPicker::countUncatalogued(m_db->connectionName(), dir.path(), false), 1);
    QCOMPARE(UploadPhotoPicker::countUncatalogued(m_db->connectionName(), dir.filePath(QStringLiteral("gone")), true), 0);
}

void TestUploadPhotoPicker::copiesOfPhotosOnINaturalistAreKnown()
{
    // Photo 2 has the same file as photo 3, which is on observation 901.
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO rendition (capture_id, path, kind, ext, content_hash) "
                                  "VALUES (2, '/lib/Copies/IMG_3 copy.JPG', 'jpeg', 'jpg', 'h3')")));
    const QList<inat::UploadPhoto> photos = inat::ObservationUploader::loadPhotos(m_db->connectionName(), {1, 2});
    for (const inat::UploadPhoto &p : photos) {
        if (p.captureId == 2) {
            QCOMPARE(p.observationId, 901);
            QVERIFY(p.linked);
            QCOMPARE(p.copyOf, QStringLiteral("IMG_3.JPG"));
        } else {
            QCOMPARE(p.observationId, 0);
            QVERIFY(!p.linked);
        }
    }
}

void TestUploadPhotoPicker::theLibraryKnowsObservationsByDay()
{
    const QList<inat::ExistingObservation> known =
        inat::InatDuplicateCheck::fromLibrary(m_db->connectionName(), {{QDate(2025, 10, 18), QDate(2025, 10, 20)}});
    QCOMPARE(known.size(), 1);
    QCOMPARE(known.at(0).id, 901);
    QCOMPARE(known.at(0).taxonId, 12936);
    QCOMPARE(known.at(0).time, QDateTime(QDate(2025, 10, 19), QTime(9, 0)));
    QCOMPARE(known.at(0).libraryPhoto, QStringLiteral("IMG_3.JPG"));
    QVERIFY(inat::InatDuplicateCheck::fromLibrary(m_db->connectionName(), {{QDate(2025, 11, 1), QDate(2025, 11, 3)}})
                .isEmpty());
}

QTEST_MAIN(TestUploadPhotoPicker)
#include "tst_uploadphotopicker.moc"
