#include <QtTest>

#include <QTemporaryDir>

#include "db/Database.h"
#include "inat/InatDownloadModel.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;
using namespace pl::taxonomy;
using namespace pl::inat;

class TestInatDownloadModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void resolvesTaxonNameFromLocalStore();
    void displayRoleJoinsNameAndDate();
    void roleNamesIncludesNameRole();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<TaxonomyStore> m_store;
    std::unique_ptr<QTemporaryDir> m_photoDir;
    std::unique_ptr<PhotoCache> m_photos;
};

void TestInatDownloadModel::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_store = std::make_unique<TaxonomyStore>(m_db->connectionName());

    Taxon t;
    t.inatId = 900;
    t.rank = QStringLiteral("species");
    t.name = QStringLiteral("Diuris sp900");
    t.commonName = QStringLiteral("Wallflower Orchid");
    QVERIFY(m_store->upsertTaxon(t) > 0);

    m_photoDir = std::make_unique<QTemporaryDir>();
    m_photos = std::make_unique<PhotoCache>(m_photoDir->path(), QByteArrayLiteral("test/1"));
}

void TestInatDownloadModel::cleanup()
{
    m_photos.reset();
    m_photoDir.reset();
    m_store.reset();
    m_db.reset();
}

namespace {
Candidate makeCandidate(qint64 obsId, qint64 taxonInatId, const QString &date, bool dup)
{
    Candidate c;
    c.observation.id = obsId;
    c.observation.taxonInatId = taxonInatId;
    c.observation.observedOn = date;
    ObservationPhoto photo;
    photo.id = obsId * 10;
    photo.previewUrl = QStringLiteral("https://x/%1/small.jpg").arg(obsId);
    photo.downloadUrl = QStringLiteral("https://x/%1/original.jpg").arg(obsId);
    c.observation.photos.append(photo);
    c.likelyDuplicate = dup;
    return c;
}
} // namespace

void TestInatDownloadModel::resolvesTaxonNameFromLocalStore()
{
    InatDownloadModel model(*m_photos, *m_store);
    model.setCandidates({makeCandidate(1, 900, QStringLiteral("2025-01-10"), false)});

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0).data(InatDownloadModel::NameRole).toString(),
             QStringLiteral("Diuris sp900"));
}

void TestInatDownloadModel::displayRoleJoinsNameAndDate()
{
    InatDownloadModel model(*m_photos, *m_store);
    model.setCandidates({makeCandidate(1, 900, QStringLiteral("2025-01-10"), false)});

    const QString text = model.index(0).data(Qt::DisplayRole).toString();
    QVERIFY(text.contains(QStringLiteral("Diuris sp900")));
    QVERIFY(text.contains(QStringLiteral("2025-01-10")));
}

void TestInatDownloadModel::roleNamesIncludesNameRole()
{
    InatDownloadModel model(*m_photos, *m_store);
    QVERIFY(model.roleNames().values().contains(QByteArray("name")));
}

QTEST_MAIN(TestInatDownloadModel)
#include "tst_inatdownloadmodel.moc"
