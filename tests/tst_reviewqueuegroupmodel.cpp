#include <QtTest>

#include <QSignalSpy>
#include <QStandardItemModel>

#include "model/ReviewQueueGroupModel.h"
#include "model/ReviewQueueModel.h"

using namespace pl;
using model::ReviewQueueGroupModel;
using model::ReviewQueueModel;

namespace {

// A stand-in source: column 0, DisplayRole = name, CaptureIdRole = a number.
QStandardItemModel *makeSource(QObject *parent, const QList<QPair<QString, int>> &rows)
{
    auto *m = new QStandardItemModel(parent);
    for (const auto &[name, id] : rows) {
        auto *item = new QStandardItem(name);
        item->setData(id, ReviewQueueModel::CaptureIdRole);
        m->appendRow(item);
    }
    return m;
}

} // namespace

class TestReviewQueueGroupModel : public QObject
{
    Q_OBJECT

private slots:
    void groupsSameNamedRowsAndKeepsLoneRowsFlat();
    void captureNavigationWalksEveryLeaf();
    void leafRowsForwardSourceRoles();
    void sourceRemovalRebuildsGroups();
    void perRowDataChangeDoesNotResetTheModel();
    void groupsByGuess();
    void groupsByFolderAndDay();
    void groupMembersListsEveryLeaf();
};

void TestReviewQueueGroupModel::groupsSameNamedRowsAndKeepsLoneRowsFlat()
{
    ReviewQueueGroupModel g;
    auto *src = makeSource(&g, {
        {QStringLiteral("Diuris pardina"), 1},
        {QStringLiteral("Diuris pardina"), 2},
        {QStringLiteral("Diuris pardina"), 3},
        {QStringLiteral("Caladenia carnea"), 4},
    });
    g.setSourceModel(src);

    QCOMPARE(g.rowCount(), 2);   // one group row + one lone row

    const QModelIndex group = g.index(0, 0);
    QVERIFY(g.isGroup(group));
    QCOMPARE(g.groupName(group), QStringLiteral("Diuris pardina"));
    QCOMPARE(g.rowCount(group), 3);
    QVERIFY(g.data(group, Qt::DisplayRole).toString().contains(QStringLiteral("(3)")));

    const QModelIndex lone = g.index(1, 0);
    QVERIFY(!g.isGroup(lone));
    QCOMPARE(g.rowCount(lone), 0);
    QCOMPARE(g.data(lone, Qt::DisplayRole).toString(), QStringLiteral("Caladenia carnea"));

    // Children hang off the group and point back to it.
    const QModelIndex child = g.index(1, 0, group);
    QVERIFY(child.isValid());
    QCOMPARE(g.parent(child), group);
    QCOMPARE(child.data(ReviewQueueModel::CaptureIdRole).toInt(), 2);
}

void TestReviewQueueGroupModel::captureNavigationWalksEveryLeaf()
{
    ReviewQueueGroupModel g;
    auto *src = makeSource(&g, {
        {QStringLiteral("A"), 10},
        {QStringLiteral("A"), 11},
        {QStringLiteral("B"), 12},
        {QStringLiteral("C"), 13},
        {QStringLiteral("C"), 14},
    });
    g.setSourceModel(src);

    QCOMPARE(g.captureCount(), 5);

    QList<int> ids;
    for (int i = 0; i < g.captureCount(); ++i) {
        const QModelIndex leaf = g.captureAt(i);
        QVERIFY(leaf.isValid());
        QVERIFY(!g.isGroup(leaf));
        QCOMPARE(g.captureNumberOf(leaf), i);
        ids << leaf.data(ReviewQueueModel::CaptureIdRole).toInt();
    }
    QCOMPARE(ids, (QList<int>{10, 11, 12, 13, 14}));

    // A group header is not a capture.
    QCOMPARE(g.captureNumberOf(g.index(0, 0)), -1);
}

void TestReviewQueueGroupModel::leafRowsForwardSourceRoles()
{
    ReviewQueueGroupModel g;
    auto *src = makeSource(&g, {{QStringLiteral("Solo"), 99}});
    g.setSourceModel(src);

    const QModelIndex lone = g.index(0, 0);
    QCOMPARE(lone.data(ReviewQueueModel::CaptureIdRole).toInt(), 99);
    QCOMPARE(lone.data(Qt::DisplayRole).toString(), QStringLiteral("Solo"));
}

void TestReviewQueueGroupModel::sourceRemovalRebuildsGroups()
{
    ReviewQueueGroupModel g;
    auto *src = makeSource(&g, {
        {QStringLiteral("A"), 1},
        {QStringLiteral("A"), 2},
        {QStringLiteral("B"), 3},
    });
    g.setSourceModel(src);
    QCOMPARE(g.rowCount(), 2);          // [A x2] , [B]
    QCOMPARE(g.captureCount(), 3);

    src->removeRow(1);                  // drop the second "A"
    QCOMPARE(g.captureCount(), 2);
    QCOMPARE(g.rowCount(), 2);          // now [A] , [B], both lone
    QVERIFY(!g.isGroup(g.index(0, 0)));
    QVERIFY(!g.isGroup(g.index(1, 0)));
}

void TestReviewQueueGroupModel::perRowDataChangeDoesNotResetTheModel()
{
    ReviewQueueGroupModel g;
    auto *src = makeSource(&g, {
        {QStringLiteral("A"), 1},
        {QStringLiteral("A"), 2},
    });
    g.setSourceModel(src);

    QSignalSpy resetSpy(&g, &QAbstractItemModel::modelReset);
    QSignalSpy dataSpy(&g, &QAbstractItemModel::dataChanged);

    src->item(0)->setData(QStringLiteral("(c) foo"), Qt::ToolTipRole);

    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(dataSpy.count() >= 1);      // leaf (and its group header) refreshed in place
}

namespace {
struct Row
{
    QString name, guess, folder;
    QDate day;
};
QStandardItemModel *makeRichSource(QObject *parent, const QList<Row> &rows)
{
    auto *m = new QStandardItemModel(parent);
    int id = 1;
    for (const Row &r : rows) {
        auto *item = new QStandardItem(r.name);
        item->setData(id++, ReviewQueueModel::CaptureIdRole);
        item->setData(r.guess, ReviewQueueModel::GuessNameRole);
        item->setData(r.folder, ReviewQueueModel::FolderPathRole);
        item->setData(r.day, ReviewQueueModel::CapturedOnRole);
        m->appendRow(item);
    }
    return m;
}
} // namespace

void TestReviewQueueGroupModel::groupsByGuess()
{
    ReviewQueueGroupModel g;
    // Three different file names, two of them guessed the same.
    g.setSourceModel(makeRichSource(&g, {
        {QStringLiteral("Pimelea 1"), QStringLiteral("Pimelea humilis"), QStringLiteral("/a"), {}},
        {QStringLiteral("DSC_0002"), QStringLiteral("Pimelea humilis"), QStringLiteral("/b"), {}},
        {QStringLiteral("Olearia"), QStringLiteral("Olearia ramulosa"), QStringLiteral("/a"), {}},
    }));
    QCOMPARE(g.rowCount(), 3);   // by name: nothing shared
    QSignalSpy resets(&g, &QAbstractItemModel::modelReset);
    g.setGroupBy(ReviewQueueGroupModel::GroupBy::Guess);
    QCOMPARE(resets.count(), 1);
    QCOMPARE(g.rowCount(), 2);
    const QModelIndex group = g.index(0, 0);
    QVERIFY(g.isGroup(group));
    QCOMPARE(g.groupName(group), QStringLiteral("Pimelea humilis"));
    QCOMPARE(g.data(group, Qt::DisplayRole).toString(), QStringLiteral("Pimelea humilis  (2)"));
    QVERIFY(!g.isGroup(g.index(1, 0)));
}

void TestReviewQueueGroupModel::groupsByFolderAndDay()
{
    ReviewQueueGroupModel g;
    const QDate d1(2016, 3, 12), d2(2016, 3, 13);
    g.setSourceModel(makeRichSource(&g, {
        {QStringLiteral("DSC_0001"), {}, QStringLiteral("/p/Unsorted 2016"), d1},
        {QStringLiteral("DSC_0002"), {}, QStringLiteral("/p/Unsorted 2016"), d1},
        {QStringLiteral("DSC_0003"), {}, QStringLiteral("/p/Unsorted 2016"), d2},
        {QStringLiteral("DSC_0004"), {}, QStringLiteral("/p/WA 2018"), d1},
        {QStringLiteral("DSC_0005"), {}, QStringLiteral("/p/WA 2018"), d1},
    }));
    g.setGroupBy(ReviewQueueGroupModel::GroupBy::FolderAndDate);
    QCOMPARE(g.rowCount(), 3);   // Unsorted d1 (2), Unsorted d2 (lone), WA d1 (2)
    const QModelIndex first = g.index(0, 0);
    QVERIFY(g.isGroup(first));
    QCOMPARE(g.groupName(first), QStringLiteral("/p/Unsorted 2016|2016-03-12"));
    QVERIFY(g.data(first, Qt::DisplayRole).toString().startsWith(QStringLiteral("Unsorted 2016 \u00b7 ")));
    QCOMPARE(g.data(first, Qt::ToolTipRole).toString(), QStringLiteral("/p/Unsorted 2016"));
    QVERIFY(!g.isGroup(g.index(1, 0)));
    QCOMPARE(g.rowCount(g.index(2, 0)), 2);
}

void TestReviewQueueGroupModel::groupMembersListsEveryLeaf()
{
    ReviewQueueGroupModel g;
    g.setSourceModel(makeSource(&g, {{QStringLiteral("A"), 1}, {QStringLiteral("A"), 2}, {QStringLiteral("B"), 3}}));
    const QModelIndexList members = g.groupMembers(g.index(0, 0));
    QCOMPARE(members.size(), 2);
    QCOMPARE(members.at(1).data(ReviewQueueModel::CaptureIdRole).toInt(), 2);
    const QModelIndexList lone = g.groupMembers(g.index(1, 0));
    QCOMPARE(lone.size(), 1);
    QCOMPARE(lone.first().data(ReviewQueueModel::CaptureIdRole).toInt(), 3);
}

QTEST_MAIN(TestReviewQueueGroupModel)
#include "tst_reviewqueuegroupmodel.moc"
