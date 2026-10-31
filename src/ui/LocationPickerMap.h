#pragma once

#include <QList>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <optional>

class QLabel;
class QToolButton;

namespace pl::net {
class TileCache;
}

namespace pl {

// The map in Set Location on Map: OpenStreetMap tiles to drag and zoom, with
// a pin the user places by clicking (or drags to move), and dots for photos
// that already have a location -- those being set, and others nearby in time
// as clues. Double-clicking a dot puts the pin there.
class LocationPickerMap : public QWidget
{
    Q_OBJECT

public:
    struct Dot
    {
        double latitude = 0;
        double longitude = 0;
        QString label;          // its tooltip
        bool selected = false;  // one of the photos being set (else a clue)
    };

    explicit LocationPickerMap(net::TileCache &tiles, QWidget *parent = nullptr);

    void setDots(const QList<Dot> &dots);
    void setPin(double latitude, double longitude);
    bool hasPin() const { return m_pin.has_value(); }
    double pinLatitude() const { return m_pin ? m_pin->y() : 0; }
    double pinLongitude() const { return m_pin ? m_pin->x() : 0; }
    void centreOn(double latitude, double longitude, int zoom = -1);
    double centreLatitude() const { return m_centerLat; }
    double centreLongitude() const { return m_centerLon; }
    int zoom() const { return m_zoom; }

    static constexpr int kMinZoom = 2;
    static constexpr int kMaxZoom = 19;

signals:
    // The user placed or moved the pin.
    void pinMoved(double latitude, double longitude);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF origin() const;   // the view's top-left, in world pixels
    QPointF toScreen(double lon, double lat) const;
    QPointF toLonLat(QPointF screen) const;   // x = longitude, y = latitude
    const Dot *dotAt(QPoint pos) const;
    void zoomAround(int zoom, QPointF screen);
    void placeOverlays();

    net::TileCache &m_tiles;
    QList<Dot> m_dots;
    std::optional<QPointF> m_pin;   // x = longitude, y = latitude
    double m_centerLon = 134.0;     // Australia, until told otherwise
    double m_centerLat = -26.0;
    int m_zoom = 4;

    enum Drag { None, Pending, Panning, MovingPin };
    Drag m_drag = None;
    QPoint m_pressPos;
    QPoint m_lastPos;
    QPointF m_grabOffset;   // from the pointer to the pin's point, while dragging it

    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_zoomOut = nullptr;
    QLabel *m_attribution = nullptr;
};

} // namespace pl
