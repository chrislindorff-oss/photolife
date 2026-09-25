#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "collection/CollectionManifest.h"
#include "collection/ExportEngine.h"
#include "db/Database.h"
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

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;
    int m_projectId = 0;

    void rescan();
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

QTEST_MAIN(TestExportEngine)
#include "tst_exportengine.moc"
