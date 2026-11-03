#include <QtTest>

#include "inat/ObservationGrouper.h"

using namespace pl;
using namespace pl::inat;

namespace {

taxonomy::Taxon species(qint64 inatId, const QString &name)
{
    taxonomy::Taxon t;
    t.inatId = inatId;
    t.rank = QStringLiteral("species");
    t.name = name;
    return t;
}

const taxonomy::Taxon kRobin = species(12936, QStringLiteral("Petroica phoenicea"));
const taxonomy::Taxon kWren = species(7000, QStringLiteral("Malurus cyaneus"));

UploadPhoto photo(int id, const taxonomy::Taxon &taxon, const QString &taken, bool gps = true)
{
    UploadPhoto p;
    p.captureId = id;
    p.label = QStringLiteral("IMG_%1.jpg").arg(id);
    p.taxon = taxon;
    if (!taken.isEmpty()) {
        p.taken = QDateTime::fromString(taken, Qt::ISODate);
        p.hasTime = taken.size() > 10;
    }
    if (gps) {
        p.latitude = -37.8 - id / 1000.0;
        p.longitude = 144.9;
    }
    return p;
}

QList<int> ids(const ProposedObservation &g)
{
    QList<int> out;
    for (const UploadPhoto &p : g.photos)
        out << p.captureId;
    return out;
}

} // namespace

class TestObservationGrouper : public QObject
{
    Q_OBJECT

private slots:
    void photosOfASpeciesWithinTheGapAreOneObservation();
    void interleavedSpeciesStaySeparate();
    void problemsAreFlagged();
    void photosAlreadyOnINaturalistStayWithTheirObservation();
    void mergeAndSplit();
    void photosWithoutASpeciesOnlyGroupInABurst();
    void photosCanBeReorderedAndMoved();
    void linkedPhotosAreDone();
    void photosFarApartCanBeSplit();
};

void TestObservationGrouper::photosOfASpeciesWithinTheGapAreOneObservation()
{
    const QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(3, kRobin, QStringLiteral("2025-10-18T09:40:00")), photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00")),
         photo(2, kRobin, QStringLiteral("2025-10-18T08:25:00"))},
        30 * 60);
    QCOMPARE(groups.size(), 2);
    QCOMPARE(ids(groups.at(0)), (QList<int>{1, 2}));   // sorted, and 25 min apart
    QCOMPARE(ids(groups.at(1)), (QList<int>{3}));      // 75 min after the last
    QCOMPARE(groups.at(0).taxon.inatId, kRobin.inatId);
    QCOMPARE(groups.at(0).observedAt(), QDateTime::fromString(QStringLiteral("2025-10-18T08:00:00"), Qt::ISODate));
    QCOMPARE(*groups.at(0).where(), QPointF(144.9, -37.801));
    QCOMPARE(groups.at(0).problems(), 0);
    QCOMPARE(groups.at(0).remaining(), 2);
}

void TestObservationGrouper::interleavedSpeciesStaySeparate()
{
    const QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00")), photo(2, kWren, QStringLiteral("2025-10-18T08:05:00")),
         photo(3, kRobin, QStringLiteral("2025-10-18T08:10:00")), photo(4, taxonomy::Taxon(), QStringLiteral("2025-10-18T08:12:00")),
         photo(5, taxonomy::Taxon(), QStringLiteral("2025-10-18T08:13:00"))},
        30 * 60);
    QCOMPARE(groups.size(), 3);
    QCOMPARE(ids(groups.at(0)), (QList<int>{1, 3}));
    QCOMPARE(ids(groups.at(1)), (QList<int>{2}));
    QCOMPARE(ids(groups.at(2)), (QList<int>{4, 5}));   // no species yet: grouped by time alone (a burst)
}

void TestObservationGrouper::problemsAreFlagged()
{
    const QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(1, taxonomy::Taxon(), QStringLiteral("2025-10-18T08:00:00")), photo(2, kRobin, QString()),
         photo(3, kWren, QStringLiteral("2025-10-19"), false), photo(4, kRobin, QString())},
        30 * 60);
    QCOMPARE(groups.size(), 4);   // undated photos each stand alone, last
    QCOMPARE(ids(groups.at(0)), (QList<int>{1}));
    QCOMPARE(groups.at(0).problems(), int(ProposedObservation::NoSpecies));
    QCOMPARE(groups.at(1).problems(), int(ProposedObservation::NoLocation));
    QVERIFY(!groups.at(1).hasTime());   // a date from the file name only
    QCOMPARE(ids(groups.at(2)), (QList<int>{2}));
    QCOMPARE(groups.at(2).problems(), int(ProposedObservation::NoDate));
    ProposedObservation dated = groups.at(2);
    dated.when = QDateTime(QDate(2025, 10, 19), QTime(7, 0));
    QCOMPARE(dated.problems(), 0);
    QVERIFY(dated.hasTime());
}

void TestObservationGrouper::photosAlreadyOnINaturalistStayWithTheirObservation()
{
    UploadPhoto sent = photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00"));
    sent.observationId = 901;
    sent.photoId = 4401;
    UploadPhoto left = photo(2, kRobin, QStringLiteral("2025-10-18T11:00:00"));   // hours later, same observation
    left.observationId = 901;
    UploadPhoto done = photo(3, kRobin, QStringLiteral("2025-10-18T08:01:00"));
    done.observationId = 902;
    done.photoId = 4402;
    const QList<ProposedObservation> groups =
        ObservationGrouper::group({sent, left, done, photo(4, kRobin, QStringLiteral("2025-10-18T08:02:00"))}, 30 * 60);
    QCOMPARE(groups.size(), 3);
    QCOMPARE(ids(groups.at(0)), (QList<int>{1, 2}));
    QCOMPARE(groups.at(0).observationId, 901);
    QCOMPARE(groups.at(0).remaining(), 1);
    QVERIFY(!groups.at(0).uploaded());
    QCOMPARE(ids(groups.at(1)), (QList<int>{3}));
    QVERIFY(groups.at(1).uploaded());
    QCOMPARE(ids(groups.at(2)), (QList<int>{4}));   // a new photo doesn't join one already made
    QCOMPARE(groups.at(2).observationId, 0);
}

void TestObservationGrouper::mergeAndSplit()
{
    QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00")), photo(2, kWren, QStringLiteral("2025-10-18T08:05:00")),
         photo(3, kRobin, QStringLiteral("2025-10-18T08:10:00"))},
        30 * 60);
    groups[0].note = QStringLiteral("On the fence");
    QCOMPARE(ObservationGrouper::speciesOf(groups).size(), 2);
    QVERIFY(ObservationGrouper::canMerge(groups));
    const ProposedObservation merged = ObservationGrouper::merge(groups, kWren);
    QCOMPARE(ids(merged), (QList<int>{1, 2, 3}));
    QCOMPARE(merged.taxon.inatId, kWren.inatId);
    QCOMPARE(merged.note, QStringLiteral("On the fence"));

    QVERIFY(!ObservationGrouper::canSplit(merged, 0));
    QVERIFY(!ObservationGrouper::canSplit(merged, 3));
    const QList<ProposedObservation> split = ObservationGrouper::splitAt(merged, 2);
    QCOMPARE(split.size(), 2);
    QCOMPARE(ids(split.at(0)), (QList<int>{1, 2}));
    QCOMPARE(ids(split.at(1)), (QList<int>{3}));
    QCOMPARE(split.at(1).taxon.inatId, kWren.inatId);
    QVERIFY(split.at(1).note.isEmpty());

    // Two observations already on iNaturalist can't become one; one can
    // take new photos, and splitting it isn't offered.
    ProposedObservation a = groups.at(0), b = groups.at(1);
    a.observationId = 901;
    QVERIFY(ObservationGrouper::canMerge({a, b}));
    QCOMPARE(ObservationGrouper::merge({a, b}, kRobin).observationId, 901);
    QVERIFY(!ObservationGrouper::canSplit(ObservationGrouper::merge({a, b}, kRobin), 1));
    b.observationId = 902;
    QVERIFY(!ObservationGrouper::canMerge({a, b}));
}

void TestObservationGrouper::photosWithoutASpeciesOnlyGroupInABurst()
{
    const taxonomy::Taxon none;
    const QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(1, none, QStringLiteral("2025-10-18T08:00:00")), photo(2, none, QStringLiteral("2025-10-18T08:01:30")),
         photo(3, none, QStringLiteral("2025-10-18T08:10:00")), photo(4, kRobin, QStringLiteral("2025-10-18T08:00:00")),
         photo(5, kRobin, QStringLiteral("2025-10-18T08:10:00"))},
        30 * 60);
    QCOMPARE(groups.size(), 3);
    QCOMPARE(ids(groups.at(0)), (QList<int>{1, 2}));   // 90 s apart
    QCOMPARE(ids(groups.at(1)), (QList<int>{4, 5}));   // a species: the 30 min gap
    QCOMPARE(ids(groups.at(2)), (QList<int>{3}));      // 8.5 min after the last unnamed one
}

void TestObservationGrouper::photosCanBeReorderedAndMoved()
{
    QList<ProposedObservation> groups = ObservationGrouper::group(
        {photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00")), photo(2, kRobin, QStringLiteral("2025-10-18T08:05:00")),
         photo(3, kRobin, QStringLiteral("2025-10-18T08:10:00")), photo(4, kWren, QStringLiteral("2025-10-18T09:00:00"))},
        30 * 60);
    QCOMPARE(groups.size(), 2);
    groups[0].checked = groups[1].checked = true;

    // The last photo first: the cover now, but the observation is still as
    // early as its earliest photo, and nothing needs looking for again.
    QVERIFY(ObservationGrouper::movePhoto(groups, 0, 2, 0, 0));
    QCOMPARE(ids(groups.at(0)), (QList<int>{3, 1, 2}));
    QCOMPARE(groups.at(0).observedAt(), QDateTime::fromString(QStringLiteral("2025-10-18T08:00:00"), Qt::ISODate));
    QCOMPARE(*groups.at(0).where(), QPointF(144.9, -37.803));   // the first with a location
    QVERIFY(groups.at(0).checked);

    // Onto the wren: it takes the wren's species, and both need checking.
    QVERIFY(ObservationGrouper::movePhoto(groups, 0, 1, 1, 0));
    QCOMPARE(ids(groups.at(0)), (QList<int>{3, 2}));
    QCOMPARE(ids(groups.at(1)), (QList<int>{1, 4}));
    QCOMPARE(groups.at(1).photos.at(0).taxon.inatId, kWren.inatId);
    QVERIFY(!groups.at(0).checked && !groups.at(1).checked);

    // A group left empty goes.
    QVERIFY(ObservationGrouper::movePhoto(groups, 0, 0, 1, 5));
    QVERIFY(ObservationGrouper::movePhoto(groups, 0, 0, 1, 5));
    QCOMPARE(groups.size(), 1);
    QCOMPARE(ids(groups.at(0)), (QList<int>{1, 4, 3, 2}));

    // Photos on iNaturalist stay put, and nothing goes ahead of them.
    ProposedObservation made = groups.at(0);
    made.observationId = 901;
    made.photos[0].observationId = 901;
    made.photos[0].photoId = 4401;
    QList<ProposedObservation> one{made};
    QVERIFY(!ObservationGrouper::canMove(one, 0, 0, 0));
    QVERIFY(ObservationGrouper::movePhoto(one, 0, 3, 0, 0));
    QCOMPARE(ids(one.at(0)), (QList<int>{1, 2, 4, 3}));
    QVERIFY(!ObservationGrouper::movePhoto(one, 0, 9, 0, 0));
}

void TestObservationGrouper::linkedPhotosAreDone()
{
    UploadPhoto linked = photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00"));
    linked.observationId = 901;
    linked.linked = true;
    UploadPhoto copy = photo(2, kRobin, QStringLiteral("2025-10-18T08:01:00"));
    copy.observationId = 901;
    copy.linked = true;
    copy.copyOf = QStringLiteral("IMG_9.jpg");
    const QList<ProposedObservation> groups = ObservationGrouper::group({linked, copy}, 30 * 60);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).remaining(), 0);
    QVERIFY(groups.at(0).uploaded());
    QVERIFY(!ObservationGrouper::canMove(groups, 0, 0, 0));
}

void TestObservationGrouper::photosFarApartCanBeSplit()
{
    UploadPhoto here = photo(1, kRobin, QStringLiteral("2025-10-18T08:00:00"));
    here.latitude = -37.83;
    UploadPhoto far = photo(2, kRobin, QStringLiteral("2025-10-18T08:10:00"));   // 10 min later, ~2 km north
    far.latitude = -37.812;
    UploadPhoto nowhere = photo(3, kRobin, QStringLiteral("2025-10-18T08:15:00"), false);
    UploadPhoto near = photo(4, taxonomy::Taxon(), QStringLiteral("2025-10-18T08:16:00"));
    UploadPhoto burst = photo(5, taxonomy::Taxon(), QStringLiteral("2025-10-18T08:17:00"));

    QCOMPARE(ObservationGrouper::group({here, far, nowhere}, 30 * 60).size(), 1);   // by time alone
    const QList<ProposedObservation> split = ObservationGrouper::group({here, far, nowhere, near, burst}, 30 * 60, 0.5);
    QCOMPARE(split.size(), 3);
    QCOMPARE(ids(split.at(0)), (QList<int>{1}));
    QCOMPARE(ids(split.at(1)), (QList<int>{2, 3}));   // no location: joins by time
    QCOMPARE(ids(split.at(2)), (QList<int>{4, 5}));   // a burst, close together
}

QTEST_GUILESS_MAIN(TestObservationGrouper)
#include "tst_observationgrouper.moc"
