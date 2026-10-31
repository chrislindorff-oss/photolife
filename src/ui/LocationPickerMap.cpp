#include "ui/LocationPickerMap.h"

#include "geo/WebMercator.h"
#include "net/TileCache.h"
#include "ui/MapTiles.h"
#include "ui/Theme.h"

#include <QHelpEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>

namespace pl {
namespace {
constexpr double kDotHitRadius = 9.0;
constexpr double kPinHitRadius = 14.0;
} // namespace

LocationPickerMap::LocationPickerMap(net::TileCache &tiles, QWidget *parent)
    : QWidget(parent), m_tiles(tiles)
{
    setMinimumSize(360, 300);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    connect(&m_tiles, &net::TileCache::ready, this, [this](int, int, int) { update(); });

    m_zoomIn = new QToolButton(this);
    m_zoomIn->setText(QStringLiteral("+"));
    m_zoomIn->setToolTip(tr("Zoom in (or the mouse wheel)"));
    m_zoomOut = new QToolButton(this);
    m_zoomOut->setText(QStringLiteral("−"));
    m_zoomOut->setToolTip(tr("Zoom out"));
    for (QToolButton *b : {m_zoomIn, m_zoomOut})
        b->setFixedSize(28, 28);
    connect(m_zoomIn, &QToolButton::clicked, this,
            [this] { zoomAround(m_zoom + 1, QPointF(width() / 2.0, height() / 2.0)); });
    connect(m_zoomOut, &QToolButton::clicked, this,
            [this] { zoomAround(m_zoom - 1, QPointF(width() / 2.0, height() / 2.0)); });

    // OpenStreetMap's tile policy asks for this on every map.
    m_attribution = new QLabel(this);
    m_attribution->setTextFormat(Qt::RichText);
    m_attribution->setText(tr("<a href=\"https://www.openstreetmap.org/copyright\">"
                              "© OpenStreetMap contributors</a>"));
    m_attribution->setOpenExternalLinks(true);
    const QColor panel = themeColors(currentThemeVariant()).panel;
    m_attribution->setStyleSheet(QStringLiteral("background-color: rgba(%1,%2,%3,190); padding: 2px 5px;")
                                     .arg(panel.red()).arg(panel.green()).arg(panel.blue()));
    m_attribution->adjustSize();
}

void LocationPickerMap::setDots(const QList<Dot> &dots)
{
    m_dots = dots;
    update();
}

void LocationPickerMap::setPin(double latitude, double longitude)
{
    m_pin = QPointF(longitude, latitude);
    update();
}

void LocationPickerMap::centreOn(double latitude, double longitude, int zoom)
{
    m_centerLat = latitude;
    m_centerLon = longitude;
    if (zoom >= 0)
        m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
    update();
}

QPointF LocationPickerMap::origin() const
{
    return geo::lonLatToWorldPixel(m_centerLon, m_centerLat, m_zoom) - QPointF(width() / 2.0, height() / 2.0);
}

QPointF LocationPickerMap::toScreen(double lon, double lat) const
{
    return geo::lonLatToWorldPixel(lon, lat, m_zoom) - origin();
}

QPointF LocationPickerMap::toLonLat(QPointF screen) const
{
    return geo::worldPixelToLonLat(origin() + screen, m_zoom);
}

const LocationPickerMap::Dot *LocationPickerMap::dotAt(QPoint pos) const
{
    for (const Dot &d : m_dots) {
        if (QLineF(toScreen(d.longitude, d.latitude), QPointF(pos)).length() <= kDotHitRadius)
            return &d;
    }
    return nullptr;
}

void LocationPickerMap::zoomAround(int zoom, QPointF screen)
{
    zoom = qBound(kMinZoom, zoom, kMaxZoom);
    if (zoom == m_zoom)
        return;
    // Keep the point under `screen` where it is.
    const QPointF at = toLonLat(screen);
    m_zoom = zoom;
    const QPointF newOrigin = geo::lonLatToWorldPixel(at.x(), at.y(), m_zoom) - screen;
    const QPointF centre = geo::worldPixelToLonLat(newOrigin + QPointF(width() / 2.0, height() / 2.0), m_zoom);
    m_centerLon = centre.x();
    m_centerLat = centre.y();
    update();
}

void LocationPickerMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0xdd, 0xdd, 0xd5));
    drawMapTiles(p, m_tiles, origin(), m_zoom, size());
    p.setRenderHint(QPainter::Antialiasing);

    // Clues first, then the photos being set on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Dot &d : std::as_const(m_dots)) {
            if (d.selected != (pass == 1))
                continue;
            p.setPen(QPen(Qt::white, 1.5));
            p.setBrush(d.selected ? QColor(0x20, 0x60, 0xd0) : QColor(0x55, 0x55, 0x55, 200));
            p.drawEllipse(toScreen(d.longitude, d.latitude), d.selected ? 7 : 5.5, d.selected ? 7 : 5.5);
        }
    }

    if (m_pin) {
        // A teardrop with its point on the spot.
        const QPointF tip = toScreen(m_pin->x(), m_pin->y());
        QPainterPath drop;
        drop.moveTo(tip);
        drop.cubicTo(tip + QPointF(-4, -10), tip + QPointF(-12, -16), tip + QPointF(-12, -25));
        drop.arcTo(QRectF(tip.x() - 12, tip.y() - 37, 24, 24), 180, -180);
        drop.cubicTo(tip + QPointF(12, -16), tip + QPointF(4, -10), tip);
        p.setPen(QPen(QColor(0x80, 0x10, 0x10), 1.5));
        p.setBrush(QColor(0xd9, 0x30, 0x25));
        p.drawPath(drop);
        p.setBrush(Qt::white);
        p.setPen(Qt::NoPen);
        p.drawEllipse(tip + QPointF(0, -25), 4.5, 4.5);
    }
}

void LocationPickerMap::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeOverlays();
}

void LocationPickerMap::placeOverlays()
{
    m_zoomIn->move(width() - 36, 8);
    m_zoomOut->move(width() - 36, 40);
    m_attribution->move(width() - m_attribution->width(), height() - m_attribution->height());
}

bool LocationPickerMap::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        if (const Dot *d = dotAt(help->pos()))
            QToolTip::showText(help->globalPos(), d->label + tr("\n(double-click to put the pin here)"), this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void LocationPickerMap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    m_pressPos = m_lastPos = event->pos();
    // On the pin's head or point: drag it; elsewhere a drag pans, a click places.
    const QPointF tip = m_pin ? toScreen(m_pin->x(), m_pin->y()) : QPointF();
    const bool onPin = m_pin && (QLineF(tip + QPointF(0, -20), event->position()).length() <= kPinHitRadius
                                 || QLineF(tip, event->position()).length() <= kPinHitRadius / 2);
    m_grabOffset = tip - event->position();
    m_drag = onPin ? MovingPin : Pending;
}

void LocationPickerMap::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton) || m_drag == None)
        return QWidget::mouseMoveEvent(event);
    if (m_drag == MovingPin) {
        m_pin = toLonLat(event->position() + m_grabOffset);
        update();
        return;
    }
    if (m_drag == Pending && (event->pos() - m_pressPos).manhattanLength() < 4)
        return;
    m_drag = Panning;
    setCursor(Qt::ClosedHandCursor);
    const QPoint delta = event->pos() - m_lastPos;
    m_lastPos = event->pos();
    const QPointF centre = geo::worldPixelToLonLat(
        geo::lonLatToWorldPixel(m_centerLon, m_centerLat, m_zoom) - QPointF(delta), m_zoom);
    m_centerLon = centre.x();
    m_centerLat = centre.y();
    update();
}

void LocationPickerMap::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(event);
    if (m_drag == Pending)   // a click: the pin goes here
        m_pin = toLonLat(event->position());
    if ((m_drag == Pending || m_drag == MovingPin) && m_pin) {
        update();
        emit pinMoved(m_pin->y(), m_pin->x());
    }
    m_drag = None;
    setCursor(Qt::CrossCursor);
}

void LocationPickerMap::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (const Dot *d = dotAt(event->pos())) {
        m_pin = QPointF(d->longitude, d->latitude);
        update();
        emit pinMoved(d->latitude, d->longitude);
        return;
    }
    // Elsewhere: zoom in there (the click before it has already placed the pin).
    zoomAround(m_zoom + 1, event->position());
}

void LocationPickerMap::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0)
        zoomAround(m_zoom + steps, event->position());
    event->accept();
}

} // namespace pl
