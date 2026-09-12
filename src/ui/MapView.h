#pragma once

#include "geo/GeoBox.h"
#include "geo/PinClusterer.h"

#include <QList>
#include <QModelIndex>
#include <QPointF>
#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;

namespace pl::model {
class CaptureListModel;
}
namespace pl::net {
class TileCache;
}

namespace pl {

// A slippy map (drag-to-pan, wheel-to-zoom) over OpenStreetMap tiles, showing
// one clustered pin per geotagged-photo location currently in `model`. This
// is purely another view of the rows CaptureListModel already exposes — no
// separate query, no separate scope — so it rebuilds its pins whenever the
// model resets (i.e. whenever whatever scopes it, such as the tree
// selection, changes).
class MapView : public QWidget
{
    Q_OBJECT

public:
    MapView(model::CaptureListModel &model, net::TileCache &tiles, QWidget *parent = nullptr);

    // The active reference tree's locality extent, or nullopt if it has none
    // (or its Place has no bbox on record). Enables/disables the "Restrict to
    // Active Reference Tree Locality" button accordingly, un-toggling it if
    // it was on and the locality just disappeared.
    void setActiveLocality(std::optional<geo::GeoBox> box);

signals:
    // The user picked one specific photo, identified by its row in `model`.
    void captureActivated(const QModelIndex &index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void rebuildPins();
    void recluster();
    void fitToPins();
    void layoutOverlays();
    QPointF centerWorldPixel() const;
    QPointF lonLatToScreen(double lon, double lat) const;
    QPointF screenToLonLat(QPointF screen) const;
    const geo::Cluster *clusterAt(QPoint pos) const;
    void activateCluster(const geo::Cluster &cluster, QPoint screenPos);

    model::CaptureListModel &m_model;
    net::TileCache &m_tiles;

    QList<geo::Pin> m_pins;
    QList<geo::Cluster> m_clusters;
    std::optional<geo::GeoBox> m_locality;

    double m_centerLon = 0.0;
    double m_centerLat = 20.0;
    int m_zoom = 2;

    bool m_dragging = false;
    QPoint m_dragLastPos;

    QPushButton *m_fitButton;
    QPushButton *m_localityButton;
    QLabel *m_attribution;
    QLabel *m_emptyOverlay;

    static constexpr int kMinZoom = 2;
    static constexpr int kMaxZoom = 18;
    static constexpr int kMaxFitZoom = 15;   // don't zoom in past this just to "fit" a single site
    static constexpr double kClusterPixelRadius = 40.0;
    static constexpr double kClusterHitRadius = 16.0;
};

} // namespace pl
