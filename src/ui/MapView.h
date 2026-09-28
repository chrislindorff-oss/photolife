#pragma once

#include "geo/GeoBox.h"
#include "geo/GeoShape.h"
#include "geo/PinClusterer.h"

#include <QList>
#include <QModelIndex>
#include <QPainterPath>
#include <QPointF>
#include <QWidget>

#include <optional>

class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPainter;
class QPushButton;
class QToolButton;

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

    // The active reference tree's locality: its extent and, once fetched,
    // its outline -- nullopt for either the tree's Place doesn't have. The
    // outline is drawn (see setShowLocality()) and, when known, is what
    // "Restrict to Active Reference Tree Locality" tests pins against; the
    // box stands in without it. Enables/disables that button accordingly,
    // un-toggling it if it was on and the locality just disappeared.
    void setActiveLocality(std::optional<geo::GeoBox> box, std::optional<geo::GeoShape> shape);

    // Whether the locality's outline is drawn (View > Show Tree Locality on Map).
    void setShowLocality(bool show);

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
    void keyPressEvent(QKeyEvent *event) override;

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

    // A multi-photo pin's panel: first the pin's taxa (with photo counts),
    // then -- after a taxon is picked -- that taxon's photos. "All taxa"
    // steps back; the close button, Esc, or a click on the map dismisses it.
    void showPanelTaxa();
    void showPanelPhotos(qint64 taxonKey);
    void onPanelItemActivated(QListWidgetItem *item);
    void placePanel();
    void setPanelTitle(const QString &text);
    void closePanel();
    bool inLocality(double lon, double lat) const;
    void paintLocality(QPainter &painter, QPointF origin);

    model::CaptureListModel &m_model;
    net::TileCache &m_tiles;

    QList<geo::Pin> m_pins;
    QList<geo::Cluster> m_clusters;
    std::optional<geo::GeoBox> m_locality;
    std::optional<geo::GeoShape> m_localityShape;
    bool m_showLocality = true;
    QPainterPath m_localityPath;       // the outline in world pixels at m_localityPathZoom
    int m_localityPathZoom = -1;       // -1: not built for the current shape

    double m_centerLon = 0.0;
    double m_centerLat = 20.0;
    int m_zoom = 2;

    bool m_dragging = false;
    QPoint m_dragLastPos;

    QPushButton *m_fitButton;
    QPushButton *m_localityButton;
    QLabel *m_attribution;
    QLabel *m_emptyOverlay;

    QFrame *m_panel;
    QToolButton *m_panelBack;
    QLabel *m_panelTitle;
    QString m_panelTitleText;          // in full; the label shows it elided
    QListWidget *m_panelList;
    QList<int> m_panelRows;            // the clicked pin's model rows
    std::optional<qint64> m_panelTaxon;   // the taxon being shown; nullopt: the taxa list
    QPoint m_panelAnchor;              // where the pin was clicked

    static constexpr int kMinZoom = 2;
    static constexpr int kMaxZoom = 18;
    static constexpr int kMaxFitZoom = 15;   // don't zoom in past this just to "fit" a single site
    static constexpr double kClusterPixelRadius = 40.0;
    static constexpr double kClusterHitRadius = 16.0;
};

} // namespace pl
