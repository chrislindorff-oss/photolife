#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "match/CandidateFinder.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "match/PathClassifier.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::match;

class TestCandidateFinder : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void forCaptureRanksExactAboveFuzzy();
    void forCaptureUsesFolderNameWhenFilenameIsOdd();
    void searchFindsByPrefixAndSynonym();
    void folderGenusResolves();

private:
    std::unique_ptr<Database> m_db;
    QTemporaryDir m_tmp;
    QString m_root;

    qint64 capture(const QString &like);
};

void TestCandidateFinder::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Lib"));

    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Caladenia carnae - Loc 1-1-2020.jpg"));  // typo
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "weird handwritten note - Loc 2-1-2020.jpg"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());
    classifyFolders(m_db->connectionName());

    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name,
                   const QStringList &syn = {}) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        t.name = name;
        t.synonyms = syn;
        store.upsertTaxon(t);
    };
    add(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    add(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"),
        {QStringLiteral("Petalochilus")});
    add(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"),
        {QStringLiteral("Petalochilus carneus")});
    add(102, 100, QStringLiteral("species"), QStringLiteral("Caladenia fuscata"));
}

void TestCandidateFinder::cleanup()
{
    m_db.reset();
}

qint64 TestCandidateFinder::capture(const QString &like)
{
    QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
    q.prepare(QStringLiteral("SELECT id FROM capture WHERE base_name LIKE ?"));
    q.addBindValue(like);
    q.exec();
    return q.next() ? q.value(0).toLongLong() : -1;
}

void TestCandidateFinder::forCaptureRanksExactAboveFuzzy()
{
    CandidateFinder finder(m_db->connectionName());
    const auto cands = finder.forCapture(capture(QStringLiteral("Caladenia carnae%")));
    QVERIFY(!cands.isEmpty());
    // Folder says "Caladenia carnea" (exact), filename is a typo -> exact wins.
    QCOMPARE(cands.first().name, QStringLiteral("Caladenia carnea"));
    QVERIFY(cands.first().score >= 0.9);
}

void TestCandidateFinder::forCaptureUsesFolderNameWhenFilenameIsOdd()
{
    CandidateFinder finder(m_db->connectionName());
    const auto cands = finder.forCapture(capture(QStringLiteral("weird handwritten note%")));
    QVERIFY(!cands.isEmpty());
    QCOMPARE(cands.first().name, QStringLiteral("Caladenia carnea"));
}

void TestCandidateFinder::searchFindsByPrefixAndSynonym()
{
    CandidateFinder finder(m_db->connectionName());

    const auto byPrefix = finder.search(QStringLiteral("Caladenia f"));
    QVERIFY(std::any_of(byPrefix.begin(), byPrefix.end(), [](const TaxonCandidate &c) {
        return c.name == QStringLiteral("Caladenia fuscata");
    }));

    const auto bySynonym = finder.search(QStringLiteral("Petalochilus carneus"));
    QVERIFY(!bySynonym.isEmpty());
    QCOMPARE(bySynonym.first().name, QStringLiteral("Caladenia carnea"));
}

void TestCandidateFinder::folderGenusResolves()
{
    CandidateFinder finder(m_db->connectionName());
    QCOMPARE(finder.folderGenusInatId(capture(QStringLiteral("weird handwritten note%"))),
             qint64(100));
}

QTEST_GUILESS_MAIN(TestCandidateFinder)
#include "tst_candidatefinder.moc"
