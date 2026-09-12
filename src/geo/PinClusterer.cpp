#include "geo/PinClusterer.h"

#include "geo/WebMercator.h"

#include <QHash>
#include <QLineF>
#include <QPair>

#include <cmath>

namespace pl::geo {
namespace {

using BucketKey = QPair<qint64, qint64>;

struct WorkingCluster
{
    QPointF pixelSum;
    int count = 0;
    QList<int> rows;
};

BucketKey bucketOf(QPointF worldPixel, double pixelRadius)
{
    return {qint64(std::floor(worldPixel.x() / pixelRadius)),
            qint64(std::floor(worldPixel.y() / pixelRadius))};
}

} // namespace

QList<Cluster> clusterPins(const QList<Pin> &pins, int zoom, double pixelRadius)
{
    QList<WorkingCluster> clusters;
    QHash<BucketKey, QList<int>> buckets;   // bucket -> indices into `clusters` touching it

    for (const Pin &pin : pins) {
        const QPointF px = lonLatToWorldPixel(pin.lon, pin.lat, zoom);
        const BucketKey key = bucketOf(px, pixelRadius);

        int found = -1;
        for (qint64 dx = -1; dx <= 1 && found < 0; ++dx) {
            for (qint64 dy = -1; dy <= 1 && found < 0; ++dy) {
                const auto it = buckets.constFind({key.first + dx, key.second + dy});
                if (it == buckets.constEnd())
                    continue;
                for (int idx : it.value()) {
                    const QPointF centroid = clusters[idx].pixelSum / clusters[idx].count;
                    if (QLineF(px, centroid).length() <= pixelRadius) {
                        found = idx;
                        break;
                    }
                }
            }
        }

        if (found < 0) {
            found = clusters.size();
            clusters.append(WorkingCluster{});
        }
        clusters[found].pixelSum += px;
        clusters[found].count += 1;
        clusters[found].rows.append(pin.row);
        buckets[key].append(found);
    }

    QList<Cluster> result;
    result.reserve(clusters.size());
    for (const WorkingCluster &w : clusters) {
        const QPointF centroidPx = w.pixelSum / w.count;
        result.append({worldPixelToLonLat(centroidPx, zoom), w.rows});
    }
    return result;
}

} // namespace pl::geo
