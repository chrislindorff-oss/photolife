#pragma once

#include <QPointF>
#include <QSize>
#include <QWidget>

class QPainter;
class QToolButton;

namespace pl::net {
class TileCache;
}

namespace pl {

// Draws the tiles `tiles` serves (street map or satellite) covering a `size`
// view whose top-left is `origin` (world pixels at `zoom`); tiles not
// downloaded yet are left out and arrive through TileCache::ready.
void drawMapTiles(QPainter &painter, net::TileCache &tiles, QPointF origin, int zoom, QSize size);

// Esri's World Imagery -- the aerial photos behind every map's Satellite view.
inline constexpr char kSatelliteTileUrl[] =
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";

// The credit each tile source asks to be shown on the map, as a link.
QString mapAttribution(bool satellite);

// A map's "Map | Satellite" choice, for a corner of the map.
class MapLayerSwitch : public QWidget
{
    Q_OBJECT

public:
    explicit MapLayerSwitch(QWidget *parent = nullptr);

    bool satellite() const;
    void setSatellite(bool satellite);   // doesn't emit

signals:
    void satelliteChanged(bool satellite);

private:
    QToolButton *m_map = nullptr;
    QToolButton *m_satellite = nullptr;
};

} // namespace pl
