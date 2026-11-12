// GpxTrack: reading a .gpx file's points and times, and finding where the
// track was at the moment a photo was taken.
#include "geo/GpxTrack.h"

#include <QBuffer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace pl::geo;

namespace {

QDateTime utc(int h, int m, int s)
{
    return QDateTime(QDate(2015, 9, 19), QTime(h, m, s), QTimeZone::utc());
}

GpxTrack parse(const QByteArray &xml, QString *error = nullptr)
{
    QByteArray copy = xml;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    return GpxTrack::read(buffer, error);
}

// As OSMTracker writes it: one segment, then a second after a 20-minute pause.
const char kTrack[] = 
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
    "<gpx xmlns=\"http://www.topografix.com/GPX/1/1\" version=\"1.1\" creator=\"test\">\n"
    "  <wpt lat=\"-37.5\" lon=\"144.5\"><name>no time: not a track point</name></wpt>\n"
    "  <trk>\n"
    "    <name><![CDATA[Tracked]]></name>\n"
    "    <trkseg>\n"
    "      <trkpt lat=\"-37.9000\" lon=\"144.2000\"><ele>267.5</ele><time>2015-09-19T07:00:00Z</time>\n"
    "        <extensions><speed>0.4</speed></extensions></trkpt>\n"
    "      <trkpt lat=\"-37.9010\" lon=\"144.2020\"><time>2015-09-19T07:00:10Z</time></trkpt>\n"
    "      <trkpt lat=\"-37.9020\" lon=\"144.2040\"><time>2015-09-19T07:00:20.500Z</time></trkpt>\n"
    "    </trkseg>\n"
    "    <trkseg>\n"
    "      <trkpt lat=\"-37.9100\" lon=\"144.2100\"><time>2015-09-19T17:20:20+10:00</time></trkpt>\n"
    "      <trkpt lat=\"-37.9110\" lon=\"144.2110\"><time>2015-09-19T07:20:30Z</time></trkpt>\n"
    "    </trkseg>\n"
    "  </trk>\n"
    "</gpx>";

} // namespace

class TestGpxTrack : public QObject
{
    Q_OBJECT

private slots:
    void readsPointsTimesAndSegments();
    void loadsFromAFile();
    void interpolatesBetweenThePointsEitherSide();
    void leavesPhotosMoreThanFiveMinutesAway();
    void aPauseInRecordingIsNotBridged();
    void aTrackWithoutTimesMatchesNothing();
    void rejectsWhatIsNotGpx();
};

void TestGpxTrack::readsPointsTimesAndSegments()
{
    QString error;
    const GpxTrack track = parse(kTrack, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!track.isEmpty());
    QCOMPARE(track.segments().size(), 2);
    QCOMPARE(track.segments().at(0).size(), 3);
    QCOMPARE(track.pointCount(), 5);
    QCOMPARE(track.timedPoints().size(), 5);
    QCOMPARE(track.start(), utc(7, 0, 0));
    QCOMPARE(track.end(), utc(7, 20, 30));
    // An offset in the file is honoured: 17:20:20+10:00 is 07:20:20 UTC.
    QCOMPARE(track.timedPoints().at(3).time, utc(7, 20, 20));
    QCOMPARE(track.segments().at(0).at(0).latitude, -37.9);
    QCOMPARE(track.segments().at(0).at(0).longitude, 144.2);
    const QRectF box = track.bounds();
    QCOMPARE(box.left(), 144.2);
    QCOMPARE(box.right(), 144.211);
    QCOMPARE(box.top(), -37.911);
    QCOMPARE(box.bottom(), -37.9);
}

void TestGpxTrack::loadsFromAFile()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("walk.gpx"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(kTrack);
    file.close();
    QString error;
    QCOMPARE(GpxTrack::load(path, &error).pointCount(), 5);
    QVERIFY(error.isEmpty());
    QVERIFY(GpxTrack::load(tmp.filePath(QStringLiteral("missing.gpx")), &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestGpxTrack::interpolatesBetweenThePointsEitherSide()
{
    const GpxTrack track = parse(kTrack);
    // On a point.
    auto at = track.positionAt(utc(7, 0, 10));
    QVERIFY(at.has_value());
    QCOMPARE(at->latitude, -37.901);
    QCOMPARE(at->longitude, 144.202);
    QCOMPARE(at->secondsAway, 0);
    // Halfway between the first two.
    at = track.positionAt(utc(7, 0, 5));
    QVERIFY(at.has_value());
    QVERIFY(qAbs(at->latitude - -37.9005) < 1e-9);
    QVERIFY(qAbs(at->longitude - 144.201) < 1e-9);
    QCOMPARE(at->secondsAway, 5);
    // A photo's local time works as well as UTC: it's the same moment.
    const auto local = track.positionAt(utc(7, 0, 5).toLocalTime());
    QVERIFY(local.has_value());
    QCOMPARE(local->latitude, at->latitude);
}

void TestGpxTrack::leavesPhotosMoreThanFiveMinutesAway()
{
    const GpxTrack track = parse(kTrack);
    // Four minutes before it started: its first point.
    auto at = track.positionAt(utc(6, 56, 0));
    QVERIFY(at.has_value());
    QCOMPARE(at->latitude, -37.9);
    QCOMPARE(at->secondsAway, 240);
    // Four minutes after it ended: its last point.
    at = track.positionAt(utc(7, 24, 30));
    QVERIFY(at.has_value());
    QCOMPARE(at->latitude, -37.911);
    // Six minutes either side: nothing.
    QVERIFY(!track.positionAt(utc(6, 54, 0)).has_value());
    QVERIFY(!track.positionAt(utc(7, 26, 31)).has_value());
    // ...unless the limit is lifted.
    QVERIFY(track.positionAt(utc(7, 26, 31), 3600).has_value());
    QCOMPARE(track.nearestInTime(utc(9, 0, 0))->latitude, -37.911);
    QVERIFY(!track.positionAt(QDateTime()).has_value());
}

void TestGpxTrack::aPauseInRecordingIsNotBridged()
{
    const GpxTrack track = parse(kTrack);
    // Recording stopped at 07:00:20 and resumed at 07:20:20. Two minutes
    // into the pause: where it stopped, not a point on the way to the resume.
    auto at = track.positionAt(utc(7, 2, 20));
    QVERIFY(at.has_value());
    QCOMPARE(at->latitude, -37.902);
    QCOMPARE(at->longitude, 144.204);
    // Two minutes before the resume: where it resumed.
    at = track.positionAt(utc(7, 18, 20));
    QVERIFY(at.has_value());
    QCOMPARE(at->latitude, -37.91);
    // The middle of the pause: more than five minutes from both.
    QVERIFY(!track.positionAt(utc(7, 10, 0)).has_value());
}

void TestGpxTrack::aTrackWithoutTimesMatchesNothing()
{
    QString error;
    const GpxTrack track = parse(
    "<gpx version=\"1.0\"><rte>\n"
    "        <rtept lat=\"-37.9\" lon=\"144.2\"/><rtept lat=\"-37.91\" lon=\"144.21\"/></rte></gpx>", &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(track.pointCount(), 2);   // still a line to draw
    QVERIFY(!track.hasTimes());
    QVERIFY(!track.start().isValid());
    QVERIFY(!track.positionAt(utc(7, 0, 0)).has_value());
    QVERIFY(!track.nearestInTime(utc(7, 0, 0)).has_value());
}

void TestGpxTrack::rejectsWhatIsNotGpx()
{
    QString error;
    QVERIFY(parse("<kml><Placemark/></kml>", &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(parse("not xml at all", &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(parse("<gpx version=\"1.1\"></gpx>", &error).isEmpty());
    QVERIFY(!error.isEmpty());   // no points
    // Cut off part-way (a tracker that died): what was read is kept, and said to be damaged.
    error.clear();
    const QByteArray whole(kTrack);
    const GpxTrack cut = parse(whole.left(whole.indexOf("</trkseg>") + 9), &error);
    QCOMPARE(cut.pointCount(), 3);
    QVERIFY(!error.isEmpty());
    // Coordinates off the globe are dropped.
    QCOMPARE(parse(
    "<gpx><trk><trkseg><trkpt lat=\"95\" lon=\"10\"/><trkpt lat=\"5\" lon=\"10\"/></trkseg></trk></gpx>")
                 .pointCount(), 1);
}

QTEST_GUILESS_MAIN(TestGpxTrack)
#include "tst_gpxtrack.moc"
