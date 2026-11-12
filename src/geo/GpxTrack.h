#pragma once

#include <QDateTime>
#include <QList>
#include <QRectF>
#include <QString>

#include <optional>

class QIODevice;

namespace pl::geo {

// One recorded position; `time` is UTC, and invalid when the file gave none.
struct TrackPoint
{
    double latitude = 0;
    double longitude = 0;
    QDateTime time;
};

// A GPS track read from a .gpx file (GPX 1.0/1.1, as phone trackers and
// handheld units write them): where its owner was, and when. Used to give a
// photo from a camera without GPS the position for the moment it was taken.
// No widgets, no catalogue; the file is only ever read.
class GpxTrack
{
public:
    // Every track point (and timed route points and waypoints). An empty
    // track with `error` set when the file can't be read or isn't GPX.
    static GpxTrack load(const QString &path, QString *error = nullptr);
    static GpxTrack read(QIODevice &device, QString *error = nullptr);

    bool isEmpty() const { return m_segments.isEmpty(); }
    int pointCount() const;
    // The lines to draw: one per track segment, in the order recorded.
    const QList<QList<TrackPoint>> &segments() const { return m_segments; }
    // The points that carry a time, oldest first: what photos are matched to.
    const QList<TrackPoint> &timedPoints() const { return m_timed; }
    bool hasTimes() const { return !m_timed.isEmpty(); }
    QDateTime start() const;   // UTC; invalid without times
    QDateTime end() const;
    // x = longitude, y = latitude (top is the southern edge).
    QRectF bounds() const;

    static constexpr int kMaxGapSeconds = 300;

    struct Match
    {
        double latitude = 0;
        double longitude = 0;
        qint64 secondsAway = 0;   // from the nearest recorded point
    };
    // Where the track was at `when`: between the points either side of it
    // when both are within `maxGapSeconds`, else the nearer one if that is;
    // nullopt when the track has nothing that close in time.
    std::optional<Match> positionAt(const QDateTime &when, int maxGapSeconds = kMaxGapSeconds) const;
    // The recorded point nearest in time to `when`, however far.
    std::optional<TrackPoint> nearestInTime(const QDateTime &when) const;

private:
    QList<QList<TrackPoint>> m_segments;
    QList<TrackPoint> m_timed;
};

} // namespace pl::geo
