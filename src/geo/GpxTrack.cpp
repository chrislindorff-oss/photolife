#include "geo/GpxTrack.h"

#include <QCoreApplication>
#include <QFile>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace pl::geo {
namespace {

QString trText(const char *text)
{
    return QCoreApplication::translate("pl::geo::GpxTrack", text);
}

// "2015-09-19T07:43:38Z", with or without fractions or an offset; a time
// with no zone at all is UTC, as GPX says it must be.
QDateTime parseTime(const QString &text)
{
    const QString t = text.trimmed();
    QDateTime dt = QDateTime::fromString(t, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(t, Qt::ISODate);
    if (!dt.isValid())
        return {};
    if (dt.timeSpec() == Qt::LocalTime)
        dt = QDateTime(dt.date(), dt.time(), QTimeZone::utc());
    return dt.toUTC();
}

// A point element's position and its <time> child; leaves the reader on its end tag.
bool readPoint(QXmlStreamReader &xml, TrackPoint &point)
{
    bool latOk = false, lonOk = false;
    point.latitude = xml.attributes().value(QLatin1String("lat")).toDouble(&latOk);
    point.longitude = xml.attributes().value(QLatin1String("lon")).toDouble(&lonOk);
    point.time = {};
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("time"))
            point.time = parseTime(xml.readElementText(QXmlStreamReader::SkipChildElements));
        else
            xml.skipCurrentElement();
    }
    return latOk && lonOk && std::abs(point.latitude) <= 90 && std::abs(point.longitude) <= 180;
}

} // namespace

GpxTrack GpxTrack::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = trText("the file couldn't be opened");
        return {};
    }
    return read(file, error);
}

GpxTrack GpxTrack::read(QIODevice &device, QString *error)
{
    GpxTrack track;
    QXmlStreamReader xml(&device);
    bool isGpx = false;
    QList<TrackPoint> loose;   // timed waypoints: places marked along the way
    // Walks the whole document: a segment's points are gathered as its
    // elements go by, wherever tracks and routes sit.
    QList<TrackPoint> segment;
    auto closeSegment = [&] {
        if (!segment.isEmpty())
            track.m_segments << segment;
        segment.clear();
    };
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView name = xml.name();
            if (name == QLatin1String("gpx")) {
                isGpx = true;
            } else if (name == QLatin1String("trkpt") || name == QLatin1String("rtept")) {
                TrackPoint p;
                if (readPoint(xml, p))
                    segment << p;
            } else if (name == QLatin1String("wpt")) {
                TrackPoint p;
                if (readPoint(xml, p) && p.time.isValid())
                    loose << p;
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView name = xml.name();
            if (name == QLatin1String("trkseg") || name == QLatin1String("trk") || name == QLatin1String("rte"))
                closeSegment();
        }
    }
    closeSegment();
    if (xml.hasError() || !isGpx) {
        if (error)
            *error = isGpx ? trText("the file is damaged (%1)").arg(xml.errorString())
                           : trText("it isn't a GPX file");
        // What was read before the damage is still a track worth having.
        if (!isGpx || track.m_segments.isEmpty())
            return {};
    }
    for (const QList<TrackPoint> &s : std::as_const(track.m_segments)) {
        for (const TrackPoint &p : s) {
            if (p.time.isValid())
                track.m_timed << p;
        }
    }
    track.m_timed << loose;
    std::stable_sort(track.m_timed.begin(), track.m_timed.end(),
                     [](const TrackPoint &a, const TrackPoint &b) { return a.time < b.time; });
    if (track.m_segments.isEmpty() && !loose.isEmpty())
        track.m_segments << track.m_timed;   // waypoints only: joined in time order
    if (track.m_segments.isEmpty() && error && error->isEmpty())
        *error = trText("it has no track points");
    return track;
}

int GpxTrack::pointCount() const
{
    int n = 0;
    for (const QList<TrackPoint> &s : m_segments)
        n += int(s.size());
    return n;
}

QDateTime GpxTrack::start() const
{
    return m_timed.isEmpty() ? QDateTime() : m_timed.first().time;
}

QDateTime GpxTrack::end() const
{
    return m_timed.isEmpty() ? QDateTime() : m_timed.last().time;
}

QRectF GpxTrack::bounds() const
{
    bool any = false;
    double west = 0, east = 0, south = 0, north = 0;
    for (const QList<TrackPoint> &s : m_segments) {
        for (const TrackPoint &p : s) {
            if (!any) {
                west = east = p.longitude;
                south = north = p.latitude;
                any = true;
                continue;
            }
            west = std::min(west, p.longitude);
            east = std::max(east, p.longitude);
            south = std::min(south, p.latitude);
            north = std::max(north, p.latitude);
        }
    }
    return any ? QRectF(QPointF(west, south), QPointF(east, north)) : QRectF();
}

std::optional<TrackPoint> GpxTrack::nearestInTime(const QDateTime &when) const
{
    if (m_timed.isEmpty() || !when.isValid())
        return std::nullopt;
    const auto after = std::lower_bound(m_timed.cbegin(), m_timed.cend(), when,
                                        [](const TrackPoint &p, const QDateTime &t) { return p.time < t; });
    if (after == m_timed.cend())
        return m_timed.last();
    if (after == m_timed.cbegin())
        return *after;
    const auto before = after - 1;
    return std::abs(before->time.secsTo(when)) <= std::abs(after->time.secsTo(when)) ? *before : *after;
}

std::optional<GpxTrack::Match> GpxTrack::positionAt(const QDateTime &when, int maxGapSeconds) const
{
    if (m_timed.isEmpty() || !when.isValid())
        return std::nullopt;
    const auto after = std::lower_bound(m_timed.cbegin(), m_timed.cend(), when,
                                        [](const TrackPoint &p, const QDateTime &t) { return p.time < t; });
    const bool hasAfter = after != m_timed.cend();
    const bool hasBefore = after != m_timed.cbegin();
    const qint64 toAfter = hasAfter ? when.msecsTo(after->time) : 0;        // >= 0
    const qint64 fromBefore = hasBefore ? (after - 1)->time.msecsTo(when) : 0;   // > 0
    const qint64 limit = qint64(maxGapSeconds) * 1000;
    const bool afterNear = hasAfter && toAfter <= limit;
    const bool beforeNear = hasBefore && fromBefore <= limit;
    if (afterNear && beforeNear) {
        const TrackPoint &a = *(after - 1);
        const TrackPoint &b = *after;
        const qint64 span = fromBefore + toAfter;
        const double f = span > 0 ? double(fromBefore) / double(span) : 0.0;
        return Match{a.latitude + (b.latitude - a.latitude) * f, a.longitude + (b.longitude - a.longitude) * f,
                     std::min(fromBefore, toAfter) / 1000};
    }
    if (afterNear)
        return Match{after->latitude, after->longitude, toAfter / 1000};
    if (beforeNear)
        return Match{(after - 1)->latitude, (after - 1)->longitude, fromBefore / 1000};
    return std::nullopt;
}

} // namespace pl::geo
