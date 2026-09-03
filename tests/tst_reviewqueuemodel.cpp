#include <QtTest>

#include <QDir>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>

#include "db/Database.h"
#include "match/MatchEngine.h"
#include "model/ReviewQueueModel.h"
#include "scan/CatalogueWriter.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"

using namespace pl;
using model::ReviewQueueModel;

class TestReviewQueueModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void searchTextRoleCombinesNameFolderAndGuess();
    void proxyFilterNarrowsTheQueue();

private:
    QTemporaryDir m_tmp;
    QString m_root;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbs;
    std::unique_ptr<ReviewQueueModel> m_model;

    int rowFor(const QString &nameContains) const;
};

void TestReviewQueueModel::init()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.filePath(QStringLiteral("Lib"));

    auto touch = [](const QString &p) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    };
    // Two Caladenia captures (one resolvable, one a mystery) and one Diuris.
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "Caladenia carnea - Anglesea 1-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Orchidaceae/Caladenia/Caladenia carnea/"
                                  "scribble note - Anglesea 2-1-2020.jpg"));
    touch(m_root + QStringLiteral("/Orchidaceae/Diuris/Diuris pardina/"
                                  "Diuris pardina - Grampians 3-1-2020.jpg"));

    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));

    scan::CatalogueWriter writer(m_db->connectionName());
    scan::FileScanner scanner;
    QVERIFY(writer.sync(scanner.scan({m_root}), {m_root}).ok());

    taxonomy::TaxonomyStore store(m_db->connectionName());
    auto add = [&](qint64 id, qint64 parent, const QString &rank, const QString &name) {
        taxonomy::Taxon t;
        t.inatId = id;
        if (parent > 0)
            t.parentInatId = parent;
        t.rank = rank;
        t.name = name;
        store.upsertTaxon(t);
    };
    add(1, 0, QStringLiteral("family"), QStringLiteral("Orchidaceae"));
    add(100, 1, QStringLiteral("genus"), QStringLiteral("Caladenia"));
    add(101, 100, QStringLiteral("species"), QStringLiteral("Caladenia carnea"));
    add(200, 1, QStringLiteral("genus"), QStringLiteral("Diuris"));
    add(201, 200, QStringLiteral("species"), QStringLiteral("Diuris pardina"));

    // Force everything into the review queue.
    match::MatchEngine engine(m_db->connectionName());
    engine.setAutoThreshold(2.0);   // nothing auto-applies
    engine.matchAll();

    m_thumbs = std::make_unique<thumb::ThumbnailCache>(m_tmp.filePath(QStringLiteral("thumbs")));
    m_model = std::make_unique<ReviewQueueModel>(*m_db, *m_thumbs);
    m_model->reload();
    QVERIFY(m_model->rowCount() >= 3);
}

void TestReviewQueueModel::cleanup()
{
    m_model.reset();
    m_thumbs.reset();
    m_db.reset();
}

int TestReviewQueueModel::rowFor(const QString &nameContains) const
{
    for (int r = 0; r < m_model->rowCount(); ++r) {
        if (m_model->index(r).data(ReviewQueueModel::SearchTextRole).toString().contains(
                nameContains, Qt::CaseInsensitive))
            return r;
    }
    return -1;
}

void TestReviewQueueModel::searchTextRoleCombinesNameFolderAndGuess()
{
    const int r = rowFor(QStringLiteral("scribble note"));
    QVERIFY(r >= 0);
    const QString text =
        m_model->index(r).data(ReviewQueueModel::SearchTextRole).toString();
    QVERIFY(text.contains(QStringLiteral("scribble note")));        // the file name
    QVERIFY(text.contains(QStringLiteral("Caladenia carnea")));     // the folder path
    // The engine's folder-based guess is also in there.
    QVERIFY(text.contains(QStringLiteral("Caladenia"), Qt::CaseInsensitive));
}

void TestReviewQueueModel::proxyFilterNarrowsTheQueue()
{
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(m_model.get());
    proxy.setFilterRole(ReviewQueueModel::SearchTextRole);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);

    const int all = proxy.rowCount();
    QVERIFY(all >= 3);

    proxy.setFilterFixedString(QStringLiteral("caladenia"));
    QCOMPARE(proxy.rowCount(), 2);   // the two Caladenia captures

    proxy.setFilterFixedString(QStringLiteral("diuris"));
    QCOMPARE(proxy.rowCount(), 1);

    proxy.setFilterFixedString(QStringLiteral("grampians"));   // matches via the base file name
    QCOMPARE(proxy.rowCount(), 1);

    proxy.setFilterFixedString(QString());
    QCOMPARE(proxy.rowCount(), all);
}

QTEST_MAIN(TestReviewQueueModel)
#include "tst_reviewqueuemodel.moc"
