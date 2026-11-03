#include <QtTest>

#include "inat/DuplicateFinder.h"

using namespace pl;
using namespace pl::inat;

namespace {

constexpr qint64 kRobin = 12936;
constexpr qint64 kPetroica = 8000;   // its genus
constexpr qint64 kWren = 7000;

// A robin seen at 08:00 on 18 Oct 2025, at -37.83, 144.98.
ProposedObservation proposed(bool withTime = true, bool withPlace = true)
{
    ProposedObservation o;
    o.taxon.inatId = kRobin;
    o.taxon.name = QStringLiteral("Petroica phoenicea");
    o.taxon.ancestry = QStringLiteral("48460/1/%1/%2").arg(kPetroica).arg(kRobin);
    UploadPhoto p;
    p.captureId = 1;
    p.taken = withTime ? QDateTime(QDate(2025, 10, 18), QTime(8, 0)) : QDateTime(QDate(2025, 10, 18), QTime(12, 0));
    p.hasTime = withTime;
    if (withPlace) {
        p.latitude = -37.83;
        p.longitude = 144.98;
    }
    o.photos << p;
    return o;
}

ExistingObservation existing(qint64 id, qint64 taxon, const QDateTime &time, std::optional<double> lat = -37.83,
                             std::optional<double> lon = 144.98)
{
    ExistingObservation e;
    e.id = id;
    e.taxonId = taxon;
    if (taxon == kRobin)
        e.taxonAncestorIds = {48460, 1, kPetroica};
    e.date = time.date();
    e.time = time;
    e.latitude = lat;
    e.longitude = lon;
    return e;
}

QList<qint64> ids(const QList<DuplicateHit> &hits)
{
    QList<qint64> out;
    for (const DuplicateHit &h : hits)
        out << h.existing.id;
    return out;
}

const QDateTime kMorning(QDate(2025, 10, 18), QTime(8, 25));

} // namespace

class TestDuplicateFinder : public QObject
{
    Q_OBJECT

private slots:
    void theSameSightingIsAHit();
    void tooFarIsNot();
    void anotherSpeciesIsNotButARelatedTaxonIs();
    void timesAreComparedWhenBothHaveOne();
    void aMissingLocationDoesNotRuleItOut();
    void theirOwnObservationIsNotAHit();
    void datesToSearchAreJoined();
};

void TestDuplicateFinder::theSameSightingIsAHit()
{
    const QList<DuplicateHit> hits =
        DuplicateFinder::find(proposed(), {existing(901, kRobin, kMorning, -37.831, 144.98)});
    QCOMPARE(ids(hits), (QList<qint64>{901}));
    QCOMPARE(hits.at(0).secondsApart, 25 * 60);
    QVERIFY(hits.at(0).km > 0.1 && hits.at(0).km < 0.12);
    QCOMPARE(hits.at(0).reasons, QStringLiteral("same species, 25 min apart, 110 m away"));
}

void TestDuplicateFinder::tooFarIsNot()
{
    // ~1.5 km north.
    QVERIFY(DuplicateFinder::find(proposed(), {existing(901, kRobin, kMorning, -37.8165, 144.98)}).isEmpty());
}

void TestDuplicateFinder::anotherSpeciesIsNotButARelatedTaxonIs()
{
    ExistingObservation subspecies = existing(904, 99999, kMorning);
    subspecies.taxonAncestorIds = {48460, 1, kPetroica, kRobin};
    const QList<DuplicateHit> hits = DuplicateFinder::find(
        proposed(), {existing(901, kWren, kMorning), existing(902, kPetroica, kMorning.addSecs(60)),
                     existing(903, 0, kMorning.addSecs(120)), subspecies});
    QCOMPARE(ids(hits).size(), 3);
    QVERIFY(!ids(hits).contains(901));   // a wren
    QVERIFY(hits.at(0).reasons.startsWith(QStringLiteral("a related taxon")));   // the genus
    QVERIFY(ids(hits).contains(903));   // not identified there: could be it
    QVERIFY(ids(hits).contains(904));   // a subspecies of it
}

void TestDuplicateFinder::timesAreComparedWhenBothHaveOne()
{
    const QDateTime evening(QDate(2025, 10, 18), QTime(19, 0));    // 11 hours later
    const QDateTime nextDay(QDate(2025, 10, 19), QTime(9, 0));     // 25 hours later
    QCOMPARE(ids(DuplicateFinder::find(proposed(), {existing(901, kRobin, evening), existing(902, kRobin, nextDay)})),
             (QList<qint64>{901}));

    // Across midnight: 23:00 and 07:00 the next day are 8 hours apart.
    ProposedObservation late = proposed();
    late.photos[0].taken = QDateTime(QDate(2025, 10, 18), QTime(23, 0));
    QCOMPARE(ids(DuplicateFinder::find(late, {existing(903, kRobin, QDateTime(QDate(2025, 10, 19), QTime(7, 0)))})),
             (QList<qint64>{903}));

    // Without a time, only the same day counts.
    ExistingObservation untimed = existing(904, kRobin, kMorning);
    untimed.time = QDateTime();
    ExistingObservation dayAfter = existing(905, kRobin, kMorning.addDays(1));
    dayAfter.time = QDateTime();
    QCOMPARE(ids(DuplicateFinder::find(proposed(), {untimed, dayAfter})), (QList<qint64>{904}));
    QCOMPARE(ids(DuplicateFinder::find(proposed(false), {existing(906, kRobin, evening), existing(907, kRobin, nextDay)})),
             (QList<qint64>{906}));
}

void TestDuplicateFinder::aMissingLocationDoesNotRuleItOut()
{
    const QList<DuplicateHit> hits = DuplicateFinder::find(proposed(), {existing(901, kRobin, kMorning, std::nullopt, std::nullopt)});
    QCOMPARE(ids(hits), (QList<qint64>{901}));
    QCOMPARE(hits.at(0).km, -1.0);
    QVERIFY(hits.at(0).reasons.endsWith(QStringLiteral("no location to compare")));
    QCOMPARE(ids(DuplicateFinder::find(proposed(true, false), {existing(902, kRobin, kMorning)})), (QList<qint64>{902}));

    // Closest first.
    QCOMPARE(ids(DuplicateFinder::find(proposed(), {existing(903, kRobin, kMorning), existing(904, kRobin, kMorning.addSecs(-1200))})),
             (QList<qint64>{904, 903}));
}

void TestDuplicateFinder::theirOwnObservationIsNotAHit()
{
    ProposedObservation part = proposed();
    part.photos[0].observationId = 901;
    QVERIFY(DuplicateFinder::find(part, {existing(901, kRobin, kMorning)}).isEmpty());
    ProposedObservation made = proposed();
    made.observationId = 902;
    QVERIFY(DuplicateFinder::find(made, {existing(902, kRobin, kMorning)}).isEmpty());
}

void TestDuplicateFinder::datesToSearchAreJoined()
{
    auto on = [](const QDate &d) {
        ProposedObservation o = proposed();
        o.photos[0].taken = QDateTime(d, QTime(8, 0));
        return o;
    };
    const QList<QPair<QDate, QDate>> ranges = DuplicateFinder::dateRanges(
        {on(QDate(2025, 10, 20)), on(QDate(2025, 10, 18)), on(QDate(2025, 12, 1)), ProposedObservation()});
    QCOMPARE(ranges.size(), 2);
    QCOMPARE(ranges.at(0), (QPair<QDate, QDate>(QDate(2025, 10, 17), QDate(2025, 10, 21))));
    QCOMPARE(ranges.at(1), (QPair<QDate, QDate>(QDate(2025, 11, 30), QDate(2025, 12, 2))));
}

QTEST_GUILESS_MAIN(TestDuplicateFinder)
#include "tst_duplicatefinder.moc"
