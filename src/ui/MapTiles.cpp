#include "ui/MapTiles.h"

#include "geo/WebMercator.h"
#include "net/TileCache.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLineF>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QToolButton>

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

QString mapAttribution(bool satellite)
{
    return satellite ? QObject::tr("<a href=\"https://www.esri.com\">Powered by Esri</a> · Esri, Maxar, "
                                   "Earthstar Geographics, and the GIS User Community")
                     : QObject::tr("<a href=\"https://www.openstreetmap.org/copyright\">"
                                   "© OpenStreetMap contributors</a>");
}

MapLayerSwitch::MapLayerSwitch(QWidget *parent)
    : QWidget(parent)
{
    m_map = new QToolButton(this);
    m_map->setText(tr("Map"));
    m_map->setToolTip(tr("The street map (OpenStreetMap)"));
    m_satellite = new QToolButton(this);
    m_satellite->setText(tr("Satellite"));
    m_satellite->setToolTip(tr("Aerial photos (Esri World Imagery)"));
    auto *group = new QButtonGroup(this);
    for (QToolButton *b : {m_map, m_satellite}) {
        b->setCheckable(true);
        b->setAutoRaise(false);
        b->setFocusPolicy(Qt::NoFocus);
        group->addButton(b);
    }
    m_map->setChecked(true);
    connect(m_satellite, &QToolButton::toggled, this, &MapLayerSwitch::satelliteChanged);

    const ThemeColors &theme = themeColors(currentThemeVariant());
    const QColor panel = theme.panel;
    setObjectName(QStringLiteral("mapLayerSwitch"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral("#mapLayerSwitch { background-color: rgba(%1,%2,%3,215); border-radius: 5px; }"
                                 "#mapLayerSwitch QToolButton { border: none; border-radius: 4px; "
                                 "padding: 3px 9px; color: %4; background: transparent; }"
                                 "#mapLayerSwitch QToolButton:checked { background: %5; color: %6; "
                                 "font-weight: 600; }")
                      .arg(panel.red()).arg(panel.green()).arg(panel.blue())
                      .arg(theme.text.name(), theme.primaryAccent.name(), panel.name()));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(2);
    layout->addWidget(m_map);
    layout->addWidget(m_satellite);
    adjustSize();
}

bool MapLayerSwitch::satellite() const
{
    return m_satellite->isChecked();
}

void MapLayerSwitch::setSatellite(bool satellite)
{
    const QSignalBlocker block(m_satellite);
    (satellite ? m_satellite : m_map)->setChecked(true);
}

} // namespace pl
