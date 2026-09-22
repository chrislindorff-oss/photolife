#include "ui/MapView.h"

#include "geo/WebMercator.h"
#include "model/CaptureListModel.h"
#include "net/TileCache.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace pl {

MapView::MapView(model::CaptureListModel &model, net::TileCache &tiles, QWidget *parent)
    : QWidget(parent), m_model(model), m_tiles(tiles)
{
    setMouseTracking(false);
    setMinimumSize(200, 200);

    connect(&m_model, &QAbstractItemModel::modelReset, this, &MapView::rebuildPins);
    connect(&m_tiles, &net::TileCache::ready, this, [this](int, int, int) { update(); });

    m_fitButton = new QPushButton(tr("Fit to Photos"), this);
    connect(m_fitButton, &QPushButton::clicked, this, [this] {
        fitToPins();
        update();
    });

    m_localityButton = new QPushButton(tr("Restrict to Active Reference Tree Locality"), this);
    m_localityButton->setCheckable(true);
    m_localityButton->setEnabled(false);   // enabled once setActiveLocality() has a box
    m_localityButton->setToolTip(
        tr("Hide map pins outside the active reference tree's locality, even if their "
           "matched species also occurs elsewhere."));
    connect(m_localityButton, &QPushButton::toggled, this, [this] { rebuildPins(); });

    m_attribution = new QLabel(this);
    m_attribution->setTextFormat(Qt::RichText);
    m_attribution->setText(tr("<a href=\"https://www.openstreetmap.org/copyright\">"
                              "© OpenStreetMap contributors</a>"));
    m_attribution->setOpenExternalLinks(true);
    {
        const QColor panel = pl::themeColors(pl::currentThemeVariant()).panel;
        m_attribution->setStyleSheet(
            QStringLiteral("background-color: rgba(%1,%2,%3,190); padding: 2px 5px;")
                .arg(panel.red()).arg(panel.green()).arg(panel.blue()));
    }
    m_attribution->adjustSize();

    m_emptyOverlay = new QLabel(tr("No geotagged photos in this selection."), this);
    m_emptyOverlay->setAlignment(Qt::AlignCenter);
    m_emptyOverlay->setWordWrap(true);
    m_emptyOverlay->setEnabled(false);
    m_emptyOverlay->hide();

    rebuildPins();
}

void MapView::rebuildPins()
{
    QList<geo::Pin> candidates;
    const int rows = m_model.rowCount();
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = m_model.index(r);
        if (!idx.data(model::CaptureListModel::HasGpsRole).toBool())
            continue;
        geo::Pin pin;
        pin.lon = idx.data(model::CaptureListModel::LongitudeRole).toDouble();
        pin.lat = idx.data(model::CaptureListModel::LatitudeRole).toDouble();
        pin.row = r;
        candidates.append(pin);
    }

    const bool restrict = m_localityButton->isChecked() && m_locality.has_value();
    if (restrict) {
        m_pins.clear();
        for (const geo::Pin &pin : candidates) {
            if (geo::boxContains(*m_locality, pin.lon, pin.lat))
                m_pins.append(pin);
        }
    } else {
        m_pins = candidates;
    }

    if (m_pins.isEmpty()) {
        m_clusters.clear();
        m_emptyOverlay->setText(restrict && !candidates.isEmpty()
                                    ? tr("No geotagged photos within this reference tree's "
                                         "locality.")
                                    : tr("No geotagged photos in this selection."));
        m_emptyOverlay->show();
    } else {
        m_emptyOverlay->hide();
        fitToPins();
    }
    layoutOverlays();
    update();
}

void MapView::setActiveLocality(std::optional<geo::GeoBox> box)
{
    m_locality = box;
    m_localityButton->setEnabled(box.has_value());
    if (!box && m_localityButton->isChecked()) {
        QSignalBlocker blocker(m_localityButton);
        m_localityButton->setChecked(false);
    }
    rebuildPins();
}

void MapView::recluster()
{
    m_clusters = geo::clusterPins(m_pins, m_zoom, kClusterPixelRadius);
}

void MapView::fitToPins()
{
    if (m_pins.isEmpty())
        return;

    double minLon = m_pins.first().lon, maxLon = minLon;
    double minLat = m_pins.first().lat, maxLat = minLat;
    for (const geo::Pin &p : m_pins) {
        minLon = std::min(minLon, p.lon);
        maxLon = std::max(maxLon, p.lon);
        minLat = std::min(minLat, p.lat);
        maxLat = std::max(maxLat, p.lat);
    }
    m_centerLon = (minLon + maxLon) / 2.0;
    m_centerLat = (minLat + maxLat) / 2.0;

    const int margin = 40;
    const double availW = std::max(1, width() - 2 * margin);
    const double availH = std::max(1, height() - 2 * margin);

    int zoom = kMaxFitZoom;
    for (; zoom > kMinZoom; --zoom) {
        const QPointF a = geo::lonLatToWorldPixel(minLon, maxLat, zoom);
        const QPointF b = geo::lonLatToWorldPixel(maxLon, minLat, zoom);
        if (std::abs(b.x() - a.x()) <= availW && std::abs(b.y() - a.y()) <= availH)
            break;
    }
    m_zoom = zoom;
    recluster();
}

void MapView::layoutOverlays()
{
    m_fitButton->adjustSize();
    m_fitButton->move(8, 8);
    m_localityButton->adjustSize();
    m_localityButton->move(8, m_fitButton->geometry().bottom() + 8);
    m_attribution->adjustSize();
    m_attribution->move(width() - m_attribution->width() - 8,
                        height() - m_attribution->height() - 8);
    m_emptyOverlay->setGeometry(rect());
    // The empty-state label covers the whole widget and would otherwise sit
    // above these buttons in sibling stacking order -- if toggling the
    // locality restriction itself empties the pin set, the user must still be
    // able to reach the button to turn it back off.
    m_fitButton->raise();
    m_localityButton->raise();
}

QPointF MapView::centerWorldPixel() const
{
    return geo::lonLatToWorldPixel(m_centerLon, m_centerLat, m_zoom);
}

QPointF MapView::lonLatToScreen(double lon, double lat) const
{
    const QPointF px = geo::lonLatToWorldPixel(lon, lat, m_zoom);
    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    return px - origin;
}

QPointF MapView::screenToLonLat(QPointF screen) const
{
    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    return geo::worldPixelToLonLat(origin + screen, m_zoom);
}

void MapView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0xdd, 0xdd, 0xd5));

    const QPointF origin = centerWorldPixel() - QPointF(width() / 2.0, height() / 2.0);
    const int tilesPerSide = 1 << m_zoom;

    const int firstTx = int(std::floor(origin.x() / geo::kTileSize));
    const int firstTy = int(std::floor(origin.y() / geo::kTileSize));
    const int lastTx = int(std::floor((origin.x() + width()) / geo::kTileSize));
    const int lastTy = int(std::floor((origin.y() + height()) / geo::kTileSize));

    m_tiles.beginFrame();
    for (int ty = firstTy; ty <= lastTy; ++ty) {
        if (ty < 0 || ty >= tilesPerSide)
            continue;
        for (int tx = firstTx; tx <= lastTx; ++tx) {
            if (tx < 0 || tx >= tilesPerSide)
                continue;
            const QPixmap pm = m_tiles.tile(m_zoom, tx, ty);
            if (pm.isNull())
                continue;
            const QPointF topLeft(tx * geo::kTileSize - origin.x(), ty * geo::kTileSize - origin.y());
            painter.drawPixmap(topLeft, pm);
        }
    }
    m_tiles.endFrame();

    painter.setRenderHint(QPainter::Antialiasing);
    for (const geo::Cluster &c : m_clusters) {
        const QPointF pos = lonLatToScreen(c.centerLonLat.x(), c.centerLonLat.y());
        const bool multi = c.rows.size() > 1;
        painter.setBrush(multi ? QColor(0xe0, 0x60, 0x20) : QColor(0x20, 0x60, 0xd0));
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawEllipse(pos, 11, 11);
        if (multi) {
            painter.setPen(Qt::white);
            painter.drawText(QRectF(pos.x() - 15, pos.y() - 10, 30, 20), Qt::AlignCenter,
                             QString::number(c.rows.size()));
        }
    }
}

void MapView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutOverlays();
}

void MapView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_dragLastPos = event->pos();
    }
    QWidget::mousePressEvent(event);
}

void MapView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        const QPoint delta = event->pos() - m_dragLastPos;
        if (!m_dragging && delta.manhattanLength() < 4)
            return;
        m_dragging = true;
        m_dragLastPos = event->pos();

        const QPointF newCenterPx = centerWorldPixel() - QPointF(delta);
        const QPointF newCenter = geo::worldPixelToLonLat(newCenterPx, m_zoom);
        m_centerLon = newCenter.x();
        m_centerLat = newCenter.y();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void MapView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_dragging) {
        if (const geo::Cluster *c = clusterAt(event->pos()))
            activateCluster(*c, event->pos());
    }
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void MapView::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;
    const int newZoom = qBound(kMinZoom, m_zoom + steps, kMaxZoom);
    if (newZoom == m_zoom)
        return;

    const QPointF cursor = event->position();
    const QPointF lonLatUnderCursor = screenToLonLat(cursor);

    m_zoom = newZoom;
    const QPointF newOrigin =
        geo::lonLatToWorldPixel(lonLatUnderCursor.x(), lonLatUnderCursor.y(), m_zoom) - cursor;
    const QPointF newCenterPx = newOrigin + QPointF(width() / 2.0, height() / 2.0);
    const QPointF newCenter = geo::worldPixelToLonLat(newCenterPx, m_zoom);
    m_centerLon = newCenter.x();
    m_centerLat = newCenter.y();

    recluster();
    update();
    event->accept();
}

const geo::Cluster *MapView::clusterAt(QPoint pos) const
{
    for (const geo::Cluster &c : m_clusters) {
        const QPointF screen = lonLatToScreen(c.centerLonLat.x(), c.centerLonLat.y());
        if (QLineF(screen, QPointF(pos)).length() <= kClusterHitRadius)
            return &c;
    }
    return nullptr;
}

void MapView::activateCluster(const geo::Cluster &cluster, QPoint screenPos)
{
    if (cluster.rows.size() == 1) {
        emit captureActivated(m_model.index(cluster.rows.first()));
        return;
    }

    QMenu menu(this);
    for (int row : cluster.rows) {
        const QModelIndex idx = m_model.index(row);
        QAction *action = menu.addAction(idx.data(Qt::DecorationRole).value<QIcon>(),
                                         idx.data(model::CaptureListModel::DisplayNameRole).toString());
        connect(action, &QAction::triggered, this, [this, row] {
            emit captureActivated(m_model.index(row));
        });
    }
    menu.exec(mapToGlobal(screenPos));
}

} // namespace pl
