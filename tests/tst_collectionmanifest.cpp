#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "collection/CollectionManifest.h"

using namespace pl::collection;

class TestCollectionManifest : public QObject
{
    Q_OBJECT

private slots:
    void roundTrips();
    void missingFileIsNullopt();
    void malformedFileIsNullopt();
};

void TestCollectionManifest::roundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CollectionManifest m;
    m.projectId = 7;
    m.projectName = QStringLiteral("Orchids of Victoria");
    m.includeCommonName = true;
    m.createdAt = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), Qt::UTC);
    m.updatedAt = QDateTime(QDate(2026, 9, 25), QTime(10, 0, 0), Qt::UTC);
    QVERIFY(m.write(dir.path()));

    const auto read = CollectionManifest::read(dir.path());
    QVERIFY(read);
    QCOMPARE(read->projectId, 7);
    QCOMPARE(read->projectName, m.projectName);
    QCOMPARE(read->includeCommonName, true);
    QCOMPARE(read->createdAt, m.createdAt);
    QCOMPARE(read->updatedAt, m.updatedAt);
}

void TestCollectionManifest::missingFileIsNullopt()
{
    QTemporaryDir dir;
    QVERIFY(!CollectionManifest::read(dir.path()));
}

void TestCollectionManifest::malformedFileIsNullopt()
{
    QTemporaryDir dir;
    QFile f(QDir(dir.path()).filePath(QLatin1String(kManifestFileName)));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not json {");
    f.close();
    QVERIFY(!CollectionManifest::read(dir.path()));

    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({"format": 1, "projectName": "no id"})");
    f.close();
    QVERIFY(!CollectionManifest::read(dir.path()));
}

QTEST_GUILESS_MAIN(TestCollectionManifest)
#include "tst_collectionmanifest.moc"
