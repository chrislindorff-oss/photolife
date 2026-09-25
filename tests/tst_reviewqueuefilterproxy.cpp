#include <QtTest>

#include <QStandardItemModel>
#include <tuple>

#include "model/ReviewQueueFilterProxy.h"
#include "model/ReviewQueueModel.h"

using namespace pl;
using model::ReviewQueueFilterProxy;
using model::ReviewQueueModel;

namespace {

// A stand-in source: column 0, DisplayRole/SearchTextRole = name, HasCandidateRole = a flag.
QStandardItemModel *makeSource(QObject *parent,
                                const QList<std::tuple<QString, bool>> &rows)
{
    auto *m = new QStandardItemModel(parent);
    for (const auto &[name, hasCandidate] : rows) {
        auto *item = new QStandardItem(name);
        item->setData(hasCandidate, ReviewQueueModel::HasCandidateRole);
        item->setData(name, ReviewQueueModel::SearchTextRole);
        m->appendRow(item);
    }
    return m;
}

} // namespace

class TestReviewQueueFilterProxy : public QObject
{
    Q_OBJECT

private slots:
    void allBucketShowsEverything();
    void noCandidateBucketNarrowsToRowsWithoutAGuess();
    void needsReviewBucketNarrowsToRowsWithAGuess();
    void bucketComposesWithAnActiveTextFilter();
};

void TestReviewQueueFilterProxy::allBucketShowsEverything()
{
    ReviewQueueFilterProxy proxy;
    auto *src = makeSource(&proxy, {
        {QStringLiteral("A"), true},
        {QStringLiteral("B"), false},
    });
    proxy.setSourceModel(src);
    proxy.setFilterRole(ReviewQueueModel::SearchTextRole);

    QCOMPARE(proxy.bucket(), ReviewQueueFilterProxy::Bucket::All);
    QCOMPARE(proxy.rowCount(), 2);
}

void TestReviewQueueFilterProxy::noCandidateBucketNarrowsToRowsWithoutAGuess()
{
    ReviewQueueFilterProxy proxy;
    auto *src = makeSource(&proxy, {
        {QStringLiteral("A"), true},
        {QStringLiteral("B"), false},
        {QStringLiteral("C"), false},
    });
    proxy.setSourceModel(src);
    proxy.setFilterRole(ReviewQueueModel::SearchTextRole);

    proxy.setBucket(ReviewQueueFilterProxy::Bucket::NoCandidate);
    QCOMPARE(proxy.rowCount(), 2);
    for (int r = 0; r < proxy.rowCount(); ++r)
        QVERIFY(!proxy.index(r, 0).data(ReviewQueueModel::HasCandidateRole).toBool());
}

void TestReviewQueueFilterProxy::needsReviewBucketNarrowsToRowsWithAGuess()
{
    ReviewQueueFilterProxy proxy;
    auto *src = makeSource(&proxy, {
        {QStringLiteral("A"), true},
        {QStringLiteral("B"), false},
    });
    proxy.setSourceModel(src);
    proxy.setFilterRole(ReviewQueueModel::SearchTextRole);

    proxy.setBucket(ReviewQueueFilterProxy::Bucket::NeedsReview);
    QCOMPARE(proxy.rowCount(), 1);
    QVERIFY(proxy.index(0, 0).data(ReviewQueueModel::HasCandidateRole).toBool());
}

void TestReviewQueueFilterProxy::bucketComposesWithAnActiveTextFilter()
{
    ReviewQueueFilterProxy proxy;
    auto *src = makeSource(&proxy, {
        {QStringLiteral("Caladenia carnea"), true},
        {QStringLiteral("Caladenia sp."), false},
        {QStringLiteral("Diuris pardina"), true},
    });
    proxy.setSourceModel(src);
    proxy.setFilterRole(ReviewQueueModel::SearchTextRole);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);

    proxy.setFilterFixedString(QStringLiteral("caladenia"));
    QCOMPARE(proxy.rowCount(), 2);

    proxy.setBucket(ReviewQueueFilterProxy::Bucket::NoCandidate);
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.index(0, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("Caladenia sp."));
}

QTEST_MAIN(TestReviewQueueFilterProxy)
#include "tst_reviewqueuefilterproxy.moc"
