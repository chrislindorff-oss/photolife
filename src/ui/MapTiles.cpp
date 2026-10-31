#include "ui/MapTiles.h"

#include "geo/WebMercator.h"
#include "net/TileCache.h"

#include <QLineF>
#include <QList>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace pl {

void drawMapTiles(QPainter &painter, net::TileCache &tiles, QPointF origin, int zoom, QSize size)
{
    const int tilesPerSide = 1 << zoom;
    const int firstTx = int(std::floor(origin.x() / geo::kTileSize));
    const int firstTy = int(std::floor(origin.y() / geo::kTileSize));
    const int lastTx = int(std::floor((origin.x() + size.width()) / geo::kTileSize));
    const int lastTy = int(std::floor((origin.y() + size.height()) / geo::kTileSize));

    // From the middle out: asked for in this order, the tiles where the eye
    // is load first.
    const QPointF middle = origin + QPointF(size.width() / 2.0, size.height() / 2.0);
    QList<QPoint> wanted;
    for (int ty = std::max(firstTy, 0); ty <= std::min(lastTy, tilesPerSide - 1); ++ty) {
        for (int tx = std::max(firstTx, 0); tx <= std::min(lastTx, tilesPerSide - 1); ++tx)
            wanted << QPoint(tx, ty);
    }
    auto distance = [middle](const QPoint &t) {
        const QPointF c((t.x() + 0.5) * geo::kTileSize, (t.y() + 0.5) * geo::kTileSize);
        return QLineF(c, middle).length();
    };
    std::sort(wanted.begin(), wanted.end(),
              [&distance](const QPoint &a, const QPoint &b) { return distance(a) < distance(b); });

    tiles.beginFrame();
    for (const QPoint &t : std::as_const(wanted)) {
        const QPixmap pm = tiles.tile(zoom, t.x(), t.y());
        if (pm.isNull())
            continue;
        painter.drawPixmap(QPointF(t.x() * geo::kTileSize - origin.x(), t.y() * geo::kTileSize - origin.y()), pm);
    }
    tiles.endFrame();
}

} // namespace pl
