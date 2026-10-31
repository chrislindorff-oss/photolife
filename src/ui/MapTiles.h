#pragma once

#include <QPointF>
#include <QSize>

class QPainter;

namespace pl::net {
class TileCache;
}

namespace pl {

// Draws the OpenStreetMap tiles covering a `size` view whose top-left is
// `origin` (world pixels at `zoom`); tiles not downloaded yet are left out
// and arrive through TileCache::ready.
void drawMapTiles(QPainter &painter, net::TileCache &tiles, QPointF origin, int zoom, QSize size);

} // namespace pl
