#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "collection/CollectionManifest.h"
#include "collection/ExportEngine.h"
#include "db/Database.h"
#include "inat/ExifWriter.h"
#include "scan/Exif.h"
#include "match/MatchEngine.h"
#include "match/MatchReviewer.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::collection;

namespace {
void write(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
}
} // namespace

class TestExportEngine : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void exportsConfirmedMatchesIntoTaxonomicFolders();
    void excludesAutoAppliedUnlessRequested();
    void scaffoldsEmptyFoldersOnlyWhenRequested();
    void disambiguatesCollisionsAndSkipsOnRerun();
    void restrictsToGivenTaxaAndReparentsExcludedOnes();
    void includesCommonNameInFolderNamesWhenRequested();
    void writesManifestAndUpdatesInPlace();
    void updateReportsFilesNoLongerInTheExport();
    void failsFastWhenDestinationIsNotWritable();
    void countsPhotosThatCouldNotBeCopied();
    void countsMissingOriginalsWithoutFailing();
    void cancelsWhilePreparingFolders();
    void reportsProgressForEachPhase();
    void leavesOutRawFilesWhenAsked();
    void updateWithoutRawDoesNotReportEarlierRawCopies();
    void exportsThumbnailsWithMetadata();
    void renamesFilesByPatternKeepingRawAndJpegPaired();
    void tellsApartPhotosThatRenderTheSameName();
    void recognisesEarlierCopiesWhenSequenceNumbersShift();
    void recordsNamingPatternInManifest();

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;
    int m_projectId = 0;

    void rescan();
    void addRawFiles();
    qint64 captureId(const QString &folderLike, const QString &baseName) const;
};

void TestExportEngine::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Library"));

    write(m_root + QStringLiteral("/Set1/Caladenia carnea/shot.jpg"), QByteArrayLiteral("AAAA"));
    write(m_root + QStringLiteral("/Set2/Caladenia carnea/shot.jpg"),
          QByteArrayLiteral("BBBBBBBB"));
    write(m_root + QStringLiteral("/Set1/Caladenia carnea/"
                                 "Caladenia carnea - Anglesea 1-1-2020.jpg"),
          QByteArrayLiteral("CCCCCCCCCCCC"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    rescan();

    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        // projectTree() orders parents-before-children by rank_level DESC, same
        // as the real iNaturalist scale (family=30, genus=20, species=10) --
        // needed here since it's what ExportEngine's folder-path walk relies on.
        if (rank == QStringLiteral("family"))
            t.rankLevel = 30;
        else if (rank == QStringLiteral("genus"))
            t.rankLevel = 20;
        else if (rank == QStringLiteral("species"))
            t.rankLevel = 10;
        t.name = name;
        store.upsertTaxon(t);
    };
    add(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    add(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    add(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    add(200, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));
    add(201, 200, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    m_projectId = store.ensureProject(QStringLiteral("Test Tree"), 1, std::nullopt,
                                      QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
    for (qint64 inatId : {1, 100, 101, 200, 201})
        QVERIFY(store.addProjectTaxon(m_projectId, inatId, true, false));
}

void TestExportEngine::cleanup()
{
    m_db.reset();
    // m_tmp outlives each test, so photos a test adds mustn't leak into the next.
    QDir(m_root).removeRecursively();
}

void TestExportEngine::rescan()
{
    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());
}

qint64 TestExportEngine::captureId(const QString &folderLike, const QString &baseName) const
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT c.id FROM capture c JOIN folder f ON f.id = c.folder_id "
        "WHERE f.path LIKE ? AND c.base_name = ?"));
    q.addBindValue(folderLike);
    q.addBindValue(baseName);
    q.exec();
    q.next();
    return q.value(0).toLongLong();
}

void TestExportEngine::exportsConfirmedMatchesIntoTaxonomicFolders()
{
    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")),
                             101, false));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-basic"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);

    QVERIFY(summary.ok());
    QCOMPARE(summary.photosCopied, 1);

    const QString expected = QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea/shot.jpg"));
    QVERIFY(QFile::exists(expected));
    QFile f(expected);
    f.open(QIODevice::ReadOnly);
    QCOMPARE(f.readAll(), QByteArrayLiteral("AAAA"));
}

void TestExportEngine::excludesAutoAppliedUnlessRequested()
{
    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();   // the clean-binomial file auto-matches with high confidence

    QSqlQuery status(QSqlDatabase::database(m_db->connectionName(), false));
    status.prepare(QStringLiteral(
        "SELECT m.status FROM capture_match m WHERE m.capture_id = ?"));
    status.addBindValue(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                  QStringLiteral("Caladenia carnea - Anglesea 1-1-2020")));
    QVERIFY(status.exec() && status.next());
    QCOMPARE(status.value(0).toString(), QStringLiteral("auto"));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-auto-filter"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions excludeAuto;
    excludeAuto.scaffoldEmptyFolders = false;
    QCOMPARE(exporter.run(m_projectId, destRoot, excludeAuto).photosCopied, 0);

    // The clean-binomial file plus both "shot.jpg"s, which auto-match by their
    // species-named folder alone.
    ExportOptions includeAuto;
    includeAuto.includeAutoApplied = true;
    includeAuto.scaffoldEmptyFolders = false;
    QCOMPARE(exporter.run(m_projectId, destRoot, includeAuto).photosCopied, 3);
}

void TestExportEngine::scaffoldsEmptyFoldersOnlyWhenRequested()
{
    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    ExportEngine exporter(m_db->connectionName());
    const QString diurisFolder = QStringLiteral("Diuris pardina");

    const QString withoutScaffold = m_tmp.filePath(QStringLiteral("no-scaffold"));
    ExportOptions off;
    off.scaffoldEmptyFolders = false;
    exporter.run(m_projectId, withoutScaffold, off);

    QVERIFY(!QDir().exists(
        QDir(withoutScaffold).filePath(QStringLiteral("Test Tree/Orchidaceae/Diuris/%1")
                                           .arg(diurisFolder))));

    const QString withScaffold = m_tmp.filePath(QStringLiteral("with-scaffold"));
    ExportOptions on;
    on.scaffoldEmptyFolders = true;
    const ExportSummary summary = exporter.run(m_projectId, withScaffold, on);
    QVERIFY(summary.ok());
    QVERIFY(QDir().exists(
        QDir(withScaffold).filePath(QStringLiteral("Test Tree/Orchidaceae/Diuris/%1")
                                        .arg(diurisFolder))));
}

void TestExportEngine::disambiguatesCollisionsAndSkipsOnRerun()
{
    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")),
                             101, false));
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set2/Caladenia carnea"),
                                       QStringLiteral("shot")),
                             101, false));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-collision"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;

    const ExportSummary first = exporter.run(m_projectId, destRoot, options);
    QVERIFY(first.ok());
    QCOMPARE(first.photosCopied, 2);
    QCOMPARE(first.photosRenamedForCollision, 1);

    const QString folder = QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea"));
    QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("shot.jpg"))));
    QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("shot (2).jpg"))));

    const ExportSummary second = exporter.run(m_projectId, destRoot, options);
    QVERIFY(second.ok());
    QCOMPARE(second.photosCopied, 0);
    QCOMPARE(second.photosSkippedExisting, 2);
}

void TestExportEngine::restrictsToGivenTaxaAndReparentsExcludedOnes()
{
    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();

    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")),
                             101, false));
    // Simulates a "genus only" confirmation -- matched directly at the genus,
    // which this test then excludes from the restricted set.
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("Caladenia carnea - Anglesea 1-1-2020")),
                             100, false));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-restricted"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;
    options.restrictToTaxonIds = {1, 101, 200, 201};   // 100 (Caladenia genus) excluded

    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);
    QVERIFY(summary.ok());
    QCOMPARE(summary.photosCopied, 2);

    // The excluded genus level is skipped entirely -- no folder for it at all.
    QVERIFY(!QDir().exists(
        QDir(destRoot).filePath(QStringLiteral("Test Tree/Orchidaceae/Caladenia"))));

    // The species (still included) attaches directly under the family.
    QVERIFY(QFile::exists(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia carnea/shot.jpg"))));

    // A photo confirmed directly at the excluded genus lands in the nearest
    // included ancestor's folder (the family) instead of being dropped.
    QVERIFY(QFile::exists(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia carnea - Anglesea 1-1-2020.jpg"))));
}

void TestExportEngine::includesCommonNameInFolderNamesWhenRequested()
{
    taxonomy::TaxonomyStore store(m_db->connectionName());
    taxonomy::Taxon t;
    t.inatId = 101;
    t.parentInatId = 100;
    t.rank = QStringLiteral("species");
    t.rankLevel = 10;
    t.name = QStringLiteral("Caladenia carnea");
    t.commonName = QStringLiteral("Pink Fingers");
    store.upsertTaxon(t);

    match::MatchEngine engine(m_db->connectionName());
    engine.matchAll();
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")),
                             101, false));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-common-name"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = true;
    options.includeCommonName = true;

    QVERIFY(exporter.run(m_projectId, destRoot, options).ok());

    // Has a common name: appended with the app's "Scientific · Common" convention.
    // sanitizeFilenameComponent() collapses the source format's doubled
    // spaces (used for visual spacing on screen) down to single spaces, as
    // it does for any run of whitespace.
    QVERIFY(QFile::exists(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea · Pink Fingers/"
                       "shot.jpg"))));

    // No common name set on Diuris pardina (init()'s fixture): suffix omitted.
    QVERIFY(QDir().exists(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Diuris/Diuris pardina"))));
}

void TestExportEngine::writesManifestAndUpdatesInPlace()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    const QString parent = m_tmp.filePath(QStringLiteral("export-update"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    const ExportSummary first = exporter.run(m_projectId, parent, options);
    QVERIFY(first.ok());
    QCOMPARE(first.photosCopied, 3);
    QCOMPARE(first.collectionRoot, QDir(parent).filePath(QStringLiteral("Test Tree")));

    const auto created = CollectionManifest::read(first.collectionRoot);
    QVERIFY(created);
    QCOMPARE(created->projectId, m_projectId);
    QCOMPARE(created->projectName, QStringLiteral("Test Tree"));

    // The user renames the collection folder; updating it in place still works.
    const QString renamed = QDir(parent).filePath(QStringLiteral("My Orchids"));
    QVERIFY(QDir().rename(first.collectionRoot, renamed));

    options.updateExisting = true;
    options.reportUnexpectedFiles = true;
    const ExportSummary update = exporter.run(m_projectId, renamed, options);
    QVERIFY(update.ok());
    QCOMPARE(update.photosCopied, 0);
    QCOMPARE(update.photosSkippedExisting, 3);
    QVERIFY2(update.unexpectedFiles.isEmpty(), qPrintable(update.unexpectedFiles.join(", ")));
    QVERIFY(!QDir(QDir(renamed).filePath(QStringLiteral("Test Tree"))).exists());   // not nested

    const auto updated = CollectionManifest::read(renamed);
    QVERIFY(updated);
    QCOMPARE(updated->createdAt, created->createdAt);
    QVERIFY(updated->updatedAt >= created->updatedAt);
}

void TestExportEngine::updateReportsFilesNoLongerInTheExport()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    const QString parent = m_tmp.filePath(QStringLiteral("export-stale"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    const QString root = exporter.run(m_projectId, parent, options).collectionRoot;

    // Since the first export: a hand-added file, and a photo reassigned elsewhere.
    write(QDir(root).filePath(QStringLiteral("notes.txt")), QByteArrayLiteral("mine"));
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("Caladenia carnea - Anglesea 1-1-2020")),
                             201, true));

    options.updateExisting = true;
    options.reportUnexpectedFiles = true;
    const ExportSummary update = exporter.run(m_projectId, root, options);
    QVERIFY(update.ok());
    QCOMPARE(update.photosCopied, 1);   // into its new Diuris pardina folder

    const QString oldCopy = QStringLiteral(
        "Orchidaceae/Caladenia/Caladenia carnea/Caladenia carnea - Anglesea 1-1-2020.jpg");
    // Sorted case-insensitively.
    QCOMPARE(update.unexpectedFiles, QStringList({QStringLiteral("notes.txt"), oldCopy}));
    QVERIFY(QFile::exists(QDir(root).filePath(oldCopy)));   // reported, never deleted
}

void TestExportEngine::failsFastWhenDestinationIsNotWritable()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    const QString readOnly = m_tmp.filePath(QStringLiteral("read-only"));
    QVERIFY(QDir().mkpath(readOnly));
    QVERIFY(QFile::setPermissions(readOnly, QFile::ReadOwner | QFile::ExeOwner));

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;

    // A new collection can't even get its "<tree name>" folder...
    const ExportSummary fresh = exporter.run(m_projectId, readOnly, options);
    QVERIFY(!fresh.ok());
    QVERIFY(fresh.error.contains(QStringLiteral("Test Tree")));
    QCOMPARE(fresh.photosCopied, 0);

    // ...and an existing one that's there but can't be written to is caught by
    // the write probe before any folder or photo is attempted.
    options.updateExisting = true;
    const ExportSummary update = exporter.run(m_projectId, readOnly, options);
    QVERIFY(!update.ok());
    QVERIFY(update.error.contains(QStringLiteral("can't write")));
    QCOMPARE(update.foldersCreated, 0);
    QCOMPARE(update.photosCopied, 0);

    QFile::setPermissions(readOnly, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void TestExportEngine::countsPhotosThatCouldNotBeCopied()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    // A file sitting where the species folder should go stops that folder --
    // and so all three of its photos -- from being created.
    const QString destRoot = m_tmp.filePath(QStringLiteral("export-blocked"));
    write(QDir(destRoot).filePath(QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea")),
          QByteArrayLiteral("in the way"));

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);

    QVERIFY(summary.ok());   // too few failures in a row to give up
    QCOMPARE(summary.photosCopied, 0);
    QCOMPARE(summary.photosFailed, 3);
    QCOMPARE(summary.failures.size(), 1);   // the folder, reported once
    QVERIFY(summary.failures.first().contains(QStringLiteral("Caladenia carnea")));
}

void TestExportEngine::countsMissingOriginalsWithoutFailing()
{
    match::MatchEngine(m_db->connectionName()).matchAll();
    QVERIFY(QFile::remove(m_root + QStringLiteral("/Set2/Caladenia carnea/shot.jpg")));

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-missing"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);

    QVERIFY(summary.ok());
    QCOMPARE(summary.photosCopied, 2);
    QCOMPARE(summary.photosFailed, 1);
    QCOMPARE(summary.failures.size(), 1);
    QVERIFY(summary.failures.first().contains(QStringLiteral("wasn't found")));
}

void TestExportEngine::cancelsWhilePreparingFolders()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-cancel"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = true;

    bool cancelRequested = false;
    const ExportSummary summary = exporter.run(
        m_projectId, destRoot, options, [&] { return cancelRequested; },
        [&](ExportEngine::Phase phase, int, int) {
            if (phase == ExportEngine::Phase::PreparingFolders)
                cancelRequested = true;
        });

    QVERIFY(summary.cancelled);
    QCOMPARE(summary.foldersCreated, 1);   // stopped right after the first folder
    QCOMPARE(summary.photosCopied, 0);
}

void TestExportEngine::reportsProgressForEachPhase()
{
    match::MatchEngine(m_db->connectionName()).matchAll();

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-progress"));
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = true;

    QList<std::tuple<ExportEngine::Phase, int, int>> last;
    auto record = [&](ExportEngine::Phase phase, int done, int total) {
        if (last.isEmpty() || std::get<0>(last.last()) != phase)
            last.append({phase, done, total});
        else
            last.last() = {phase, done, total};
    };
    QVERIFY(exporter.run(m_projectId, destRoot, options, {}, record).ok());

    QCOMPARE(last.size(), 2);
    QCOMPARE(last[0], std::make_tuple(ExportEngine::Phase::PreparingFolders, 5, 5));
    QCOMPARE(last[1], std::make_tuple(ExportEngine::Phase::CopyingPhotos, 3, 3));
}

// A RAW beside an existing JPEG (one photo, two files) and a photo that has
// only a RAW file, both in the Caladenia carnea folder the matcher uses.
void TestExportEngine::addRawFiles()
{
    write(m_root + QStringLiteral("/Set1/Caladenia carnea/shot.CR3"), QByteArrayLiteral("RAWRAWRAW"));
    write(m_root + QStringLiteral("/Set1/Caladenia carnea/rawonly.NEF"),
          QByteArrayLiteral("NEFNEFNEFNEF"));
    rescan();
    match::MatchEngine(m_db->connectionName()).matchAll();
}

void TestExportEngine::leavesOutRawFilesWhenAsked()
{
    addRawFiles();
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    const QString species = QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea");

    const QString withRaw = m_tmp.filePath(QStringLiteral("export-with-raw"));
    const ExportSummary all = exporter.run(m_projectId, withRaw, options);
    QVERIFY(all.ok());
    QCOMPARE(all.photosCopied, 5);   // 3 JPEGs + 2 RAWs
    QVERIFY(QFile::exists(QDir(withRaw).filePath(species + QStringLiteral("/shot.CR3"))));
    QVERIFY(QFile::exists(QDir(withRaw).filePath(species + QStringLiteral("/rawonly.NEF"))));

    options.includeRaw = false;
    const QString noRaw = m_tmp.filePath(QStringLiteral("export-no-raw"));
    const ExportSummary jpegs = exporter.run(m_projectId, noRaw, options);
    QVERIFY(jpegs.ok());
    QCOMPARE(jpegs.photosCopied, 3);
    QCOMPARE(jpegs.photosFailed, 0);   // RAW-only photos are left out, not failures
    QVERIFY(QFile::exists(QDir(noRaw).filePath(species + QStringLiteral("/shot.jpg"))));
    QVERIFY(!QFile::exists(QDir(noRaw).filePath(species + QStringLiteral("/shot.CR3"))));
    QVERIFY(!QFile::exists(QDir(noRaw).filePath(species + QStringLiteral("/rawonly.NEF"))));
}

void TestExportEngine::updateWithoutRawDoesNotReportEarlierRawCopies()
{
    addRawFiles();
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    const QString root =
        exporter.run(m_projectId, m_tmp.filePath(QStringLiteral("export-raw-update")), options)
            .collectionRoot;

    options.includeRaw = false;
    options.updateExisting = true;
    options.reportUnexpectedFiles = true;
    const ExportSummary update = exporter.run(m_projectId, root, options);
    QVERIFY(update.ok());
    QCOMPARE(update.photosSkippedExisting, 3);
    QVERIFY(update.unexpectedFiles.isEmpty());
}

void TestExportEngine::exportsThumbnailsWithMetadata()
{
    // A real, decodable photo carrying a date and GPS. The fixture's other
    // three "photos" are just bytes, so they can't be made into thumbnails.
    const QString real = m_root + QStringLiteral("/Set3/Caladenia carnea/real.jpg");
    QDir().mkpath(QFileInfo(real).absolutePath());
    QImage image(800, 600, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QVERIFY(image.save(real, "JPG", 95));
    inat::ExifFields fields;
    fields.dateTimeOriginal = QStringLiteral("2024-03-15");
    fields.latitude = -37.5;
    fields.longitude = 144.9;
    QVERIFY(inat::writeExif(real, fields));
    rescan();
    match::MatchEngine(m_db->connectionName()).matchAll();

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    options.thumbnailPx = 256;
    const QString destRoot = m_tmp.filePath(QStringLiteral("export-thumbs"));
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);

    QVERIFY(summary.ok());
    QCOMPARE(summary.photosCopied, 1);
    QCOMPARE(summary.photosFailed, 3);   // the undecodable ones -- not fatal
    QVERIFY(summary.failures.first().contains(QStringLiteral("couldn't make a thumbnail")));

    const QString thumb = QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea/real.jpg"));
    QImageReader reader(thumb);
    QCOMPARE(reader.size(), QSize(256, 192));
    QVERIFY(QFileInfo(thumb).size() < QFileInfo(real).size());

    const scan::ExifData exif = scan::readJpegExif(thumb);
    QCOMPARE(exif.dateTimeOriginal.date(), QDate(2024, 3, 15));
    QVERIFY(exif.latitude && qAbs(*exif.latitude - -37.5) < 1e-3);
    QVERIFY(exif.longitude && qAbs(*exif.longitude - 144.9) < 1e-3);

    const auto manifest = CollectionManifest::read(summary.collectionRoot);
    QVERIFY(manifest);
    QCOMPARE(manifest->thumbnailPx, 256);

    // The same photo gives the same bytes, so a rerun recognises it.
    const ExportSummary again = exporter.run(m_projectId, destRoot, options);
    QCOMPARE(again.photosCopied, 0);
    QCOMPARE(again.photosSkippedExisting, 1);
    QCOMPARE(again.photosRenamedForCollision, 0);
}

namespace {
FileNamePattern patternOf(const QList<NameToken> &tokens, bool spacesToSeparator = true)
{
    FileNamePattern p;
    for (NameToken t : tokens)
        p.parts.append({t, {}});
    p.spacesToSeparator = spacesToSeparator;
    return p;
}

QByteArray contentOf(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}
} // namespace

void TestExportEngine::renamesFilesByPatternKeepingRawAndJpegPaired()
{
    addRawFiles();   // shot.CR3 beside Set1's shot.jpg, plus a RAW-only photo
    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.includeAutoApplied = true;
    options.scaffoldEmptyFolders = false;
    options.fileNamePattern = patternOf({NameToken::ScientificName, NameToken::Sequence});

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-renamed"));
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);
    QVERIFY(summary.ok());
    QCOMPARE(summary.photosCopied, 5);
    QCOMPARE(summary.photosRenamedForCollision, 0);

    const QDir folder(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea")));
    const QStringList files = folder.entryList(QDir::Files, QDir::Name);
    QCOMPARE(files.size(), 5);
    for (const QString &file : files)
        QVERIFY2(file.startsWith(QStringLiteral("Caladenia_carnea_00")), qPrintable(file));

    // Four photos, numbered 001-004; the RAW keeps its JPEG's number.
    const QStringList raws = folder.entryList({QStringLiteral("*.CR3")}, QDir::Files);
    QCOMPARE(raws.size(), 1);
    const QString jpeg = QFileInfo(raws.first()).completeBaseName() + QStringLiteral(".jpg");
    QCOMPARE(contentOf(folder.filePath(jpeg)), QByteArrayLiteral("AAAA"));
    QVERIFY(QFile::exists(folder.filePath(QStringLiteral("Caladenia_carnea_004.jpg")))
            || QFile::exists(folder.filePath(QStringLiteral("Caladenia_carnea_004.NEF"))));

    // The library is untouched.
    QVERIFY(QFile::exists(m_root + QStringLiteral("/Set1/Caladenia carnea/shot.CR3")));
    QVERIFY(QFile::exists(m_root + QStringLiteral("/Set1/Caladenia carnea/shot.jpg")));

    // Same names on a rerun, so nothing is copied twice.
    const ExportSummary again = exporter.run(m_projectId, destRoot, options);
    QVERIFY(again.ok());
    QCOMPARE(again.photosCopied, 0);
    QCOMPARE(again.photosSkippedExisting, 5);
}

void TestExportEngine::tellsApartPhotosThatRenderTheSameName()
{
    match::MatchEngine(m_db->connectionName()).matchAll();
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")), 101, false));
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set2/Caladenia carnea"),
                                       QStringLiteral("shot")), 101, false));

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;
    options.fileNamePattern = patternOf({NameToken::ScientificName}, false);

    const QString destRoot = m_tmp.filePath(QStringLiteral("export-same-name"));
    const ExportSummary first = exporter.run(m_projectId, destRoot, options);
    QVERIFY(first.ok());
    QCOMPARE(first.photosCopied, 2);
    QCOMPARE(first.photosRenamedForCollision, 1);

    const QDir folder(QDir(destRoot).filePath(
        QStringLiteral("Test Tree/Orchidaceae/Caladenia/Caladenia carnea")));
    QCOMPARE(contentOf(folder.filePath(QStringLiteral("Caladenia carnea.jpg"))),
             QByteArrayLiteral("AAAA"));
    QCOMPARE(contentOf(folder.filePath(QStringLiteral("Caladenia carnea (2).jpg"))),
             QByteArrayLiteral("BBBBBBBB"));

    const ExportSummary second = exporter.run(m_projectId, destRoot, options);
    QCOMPARE(second.photosCopied, 0);
    QCOMPARE(second.photosSkippedExisting, 2);
}

void TestExportEngine::recognisesEarlierCopiesWhenSequenceNumbersShift()
{
    match::MatchEngine(m_db->connectionName()).matchAll();
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set2/Caladenia carnea"),
                                       QStringLiteral("shot")), 101, false));

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;
    options.fileNamePattern = patternOf({NameToken::ScientificName, NameToken::Sequence});
    const QString destRoot = m_tmp.filePath(QStringLiteral("export-shift"));
    QCOMPARE(exporter.run(m_projectId, destRoot, options).photosCopied, 1);

    // A photo confirmed since sorts ahead of it and takes its number.
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")), 101, false));
    options.updateExisting = true;
    options.reportUnexpectedFiles = true;
    const QString collection = QDir(destRoot).filePath(QStringLiteral("Test Tree"));
    const ExportSummary update = exporter.run(m_projectId, collection, options);
    QVERIFY(update.ok());
    QCOMPARE(update.photosCopied, 1);
    QCOMPARE(update.photosSkippedExisting, 1);
    QVERIFY2(update.unexpectedFiles.isEmpty(), qPrintable(update.unexpectedFiles.join(", ")));

    const QDir folder(QDir(collection).filePath(
        QStringLiteral("Orchidaceae/Caladenia/Caladenia carnea")));
    QSet<QByteArray> contents;
    const QStringList files = folder.entryList(QDir::Files);
    for (const QString &file : files)
        contents.insert(contentOf(folder.filePath(file)));
    QCOMPARE(files.size(), 2);   // no second copy of the first photo
    QCOMPARE(contents, (QSet<QByteArray>{QByteArrayLiteral("AAAA"), QByteArrayLiteral("BBBBBBBB")}));
}

void TestExportEngine::recordsNamingPatternInManifest()
{
    match::MatchEngine(m_db->connectionName()).matchAll();
    match::MatchReviewer reviewer(m_db->connectionName());
    QVERIFY(reviewer.confirm(captureId(QStringLiteral("%Set1/Caladenia carnea"),
                                       QStringLiteral("shot")), 101, false));

    ExportEngine exporter(m_db->connectionName());
    ExportOptions options;
    options.scaffoldEmptyFolders = false;
    options.fileNamePattern = patternOf({NameToken::ScientificName, NameToken::Author});
    options.authorName = QStringLiteral("C Lee");
    const QString destRoot = m_tmp.filePath(QStringLiteral("export-manifest-pattern"));
    const ExportSummary summary = exporter.run(m_projectId, destRoot, options);
    QVERIFY(summary.ok());
    QVERIFY(QFile::exists(QDir(summary.collectionRoot)
                              .filePath(QStringLiteral("Orchidaceae/Caladenia/Caladenia carnea/"
                                                       "Caladenia_carnea_C_Lee.jpg"))));

    const auto manifest = CollectionManifest::read(summary.collectionRoot);
    QVERIFY(manifest);
    QCOMPARE(manifest->fileNamePattern, options.fileNamePattern);
    QCOMPARE(manifest->authorName, QStringLiteral("C Lee"));
}

QTEST_MAIN(TestExportEngine)
#include "tst_exportengine.moc"
